/* Stage 3 native<->service integration test.
 *
 * Links the authoritative service, the native parser and the native remote in
 * one process: the remote's trusted JSON commands are fed through the real
 * private command channel, the service's real on-disk public display is read
 * back, and the native view parser is checked to agree, field for field, with
 * the reference JSON parser over every phase it passes through. Covers normal
 * correct/incorrect judging, daily doubles, pause/resume, restart and the
 * two-phase quit confirmation. */
#include "../src/jeopardy_service.h"
#include "../src/jpq_view.h"
#include "../src/jpq_remote.h"
#include "../../../reboxd/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

static int CHECKS, FAILS;
static void check(int cond, const char *msg) {
    CHECKS++;
    if (!cond) { FAILS++; printf("FAIL: %s\n", msg); }
}

static uint64_t mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int read_file(const char *path, char *buf, size_t cap, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[got] = 0;
    if (n) *n = got;
    return 0;
}

static char g_dir[1024];

static void send_line(JpqService *s, const char *line) {
    jpq_service_line(s, 1, line, strlen(line));
}

/* Compare the native parser against the reference DOM for the same bytes. */
static void assert_match(const JpqView *v, const char *raw, size_t n) {
    struct jval *root = json_parse(raw, n);
    check(root && root->t == J_OBJ, "public display parses");
    struct jval *st = jget(root, "state");
    check(st && st->t == J_OBJ, "public display has state");
    if (!st) { jfree(root); return; }
    check((int)jnum(jget(st, "phase"), -1) == (int)v->phase, "phase matches core");
    check((int)jnum(jget(st, "round"), -1) == (int)v->round, "round matches core");
    check((int)jnum(jget(st, "buzzed"), -99) == (int)v->buzzed, "buzzed slot matches core");
    check((int)jnum(jget(st, "selector"), -99) == (int)v->selector, "selector matches core");
    check((int)jnum(jget(st, "dd_selector"), -99) == (int)v->dd_selector, "dd_selector matches core");
    check((int)jnum(jget(st, "paused"), -1) == (int)v->paused, "paused matches core");
    struct jval *players = jget(st, "players");
    check(players && (int)players->n == v->player_count, "player count matches core");
    if (players) {
        for (int i = 0; i < v->player_count && i < (int)players->n; i++) {
            struct jval *p = jnth(players, i);
            check((int)jnum(jget(p, "id"), -1) == (int)v->players[i].id, "player id matches");
            check((int)jnum(jget(p, "score"), 0) == (int)v->players[i].score, "player score matches");
            int has = jget(p, "answer")->t == J_STR;
            check(has == v->players[i].has_answer, "player answer redaction flag matches");
        }
    }
    /* colours ride in the wrapper and must reach the parsed podium colour */
    struct jval *colors = jget(root, "colors");
    check(colors && colors->t == J_OBJ, "public display has colours");
    if (colors && v->player_count) {
        char key[16];
        snprintf(key, sizeof key, "%u", v->players[0].id);
        const char *c0 = jstr(jget(colors, key));
        check(c0 && !strcmp(c0, v->players[0].color), "podium colour matches service");
    }
    jfree(root);
}

static void sync_view(JpqView *v) {
    char path[1200], buf[262144];
    size_t n = 0;
    snprintf(path, sizeof path, "%s/native/public-display.json", g_dir);
    check(read_file(path, buf, sizeof buf, &n) == 0, "public display readable");
    check(jpq_view_parse(v, buf, n) == 0, "native parser accepts service snapshot");
    assert_match(v, buf, n);
}

static void tick(JpqService *s) { jpq_service_tick(s, mono()); }

static void wait_phase(JpqService *s, JpqView *v, int phase, int timeout_ms) {
    uint64_t end = mono() + timeout_ms;
    for (;;) {
        sync_view(v);
        if (v->phase == phase) return;
        if (mono() >= end) return;
        tick(s);
        struct timespec d = {0, 5000000};
        nanosleep(&d, 0);
    }
}

static void remote_do(JpqService *s, JpqRemote *r, const JpqView *v, JvKey k, const char *what) {
    char out[512];
    int rc = jpq_remote_key(r, k, v, out, sizeof out);
    check(rc >= 0, what);
    if (rc == 1) send_line(s, out);
}

static void host_reveal(JpqService *s, JpqRemote *r, const JpqView *v) {
    r->menu = JRM_SETTINGS;
    r->cursor = 6;
    remote_do(s, r, v, JV_KEY_OK, "host reveal via menu");
    r->menu = JRM_NONE;
}

int main(int argc, char **argv) {
    (void)argc;
    const char *exe_dir = argv[0] ? argv[0] : ".";
    (void)exe_dir;
    const char *db = argv[1] ? argv[1] : "../build/demo.jdb";

    snprintf(g_dir, sizeof g_dir, "/tmp/jpq-int-XXXXXX");
    check(mkdtemp(g_dir) != NULL, "temp data dir");

    JpqService *svc = jpq_service_create();
    check(svc != NULL, "service create");
    check(jpq_service_start(svc, g_dir, db) == 0, "service start");
    check(jpq_service_set_port(svc, 0, "http://127.0.0.1:1/") == 0, "metadata written");

    /* private PIN for join, read exactly like the native renderer */
    char pin[16] = "0000";
    {
        char path[1200], buf[8192];
        size_t n = 0;
        JpqMetadata meta;
        snprintf(path, sizeof path, "%s/native/metadata.json", g_dir);
        if (read_file(path, buf, sizeof buf, &n) == 0 && jpq_metadata_parse(&meta, buf, n) == 0)
            snprintf(pin, sizeof pin, "%s", meta.join_pin);
    }
    check(strlen(pin) == 4, "join pin issued");

    /* three players join over HTTP, then ready via the native channel */
    uint32_t id[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        char body[128], *resp;
        snprintf(body, sizeof body, "{\"name\":\"P%d\",\"pin\":\"%s\"}", i + 1, pin);
        JpqHttpRequest req;
        memset(&req, 0, sizeof req);
        strcpy(req.method, "POST");
        strcpy(req.path, "/api/join");
        req.body = body;
        req.body_len = strlen(body);
        size_t len = 0;
        const char *ct = NULL;
        int status = 200;
        resp = jpq_service_http(svc, &req, &len, &ct, &status);
        check(status == 200 && resp, "player join over HTTP");
        struct jval *j = resp ? json_parse(resp, strlen(resp)) : NULL;
        id[i] = j ? (uint32_t)jnum(jget(j, "playerId"), 0) : 0;
        jfree(j);
        free(resp);
        check(id[i] > 0, "player id issued");
        char line[96];
        snprintf(line, sizeof line, "{\"actor\":%u,\"action\":\"READY\",\"value\":1}", id[i]);
        send_line(svc, line);
    }

    /* fast, deterministic timers */
    send_line(svc,
        "{\"actor\":\"host\",\"action\":\"CONFIG\",\"config\":{"
        "\"read_ms\":10,\"buzz_ms\":5000,\"answer_ms\":5000,\"dd_wager_ms\":3000,"
        "\"dd_answer_ms\":3000,\"final_wager_ms\":3000,\"final_answer_ms\":3000,"
        "\"tiebreak_answer_ms\":3000}}");

    JpqView v;
    JpqRemote r;
    jpq_remote_init(&r);
    wait_phase(svc, &v, JV_LOBBY, 1000);
    check(v.phase == JV_LOBBY && v.player_count == 3, "lobby with three players");
    check(v.players[0].has_answer == 0, "no public answer in lobby");

    /* START via the native remote */
    {
        char out[512];
        check(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1, "remote START");
        send_line(svc, out);
    }
    wait_phase(svc, &v, JV_BOARD, 2000);
    check(v.phase == JV_BOARD, "board after start");

    int did_correct = 0, did_incorrect = 0, did_dd = 0;
    for (int pass = 0; pass < 60 && !(did_correct && did_incorrect && did_dd); pass++) {
        /* pick the next unused cell */
        int col = -1, row = -1;
        for (int c = 0; c < JPQ_VIEW_COLS && col < 0; c++)
            for (int rr = 0; rr < JPQ_VIEW_ROWS; rr++)
                if (!v.categories[c].used[rr]) { col = c; row = rr; break; }
        if (col < 0) break;
        r.col = col; r.row = row;
        remote_do(svc, &r, &v, JV_KEY_OK, "select clue");
        sync_view(&v);
        if (v.phase != JV_CLUE && v.phase != JV_DD_WAGER) { tick(svc); sync_view(&v); }

        if (v.phase == JV_DD_WAGER) {
            int dsel = v.dd_selector;
            check(dsel >= 0 && dsel < v.player_count, "dd_selector valid");
            char line[96];
            snprintf(line, sizeof line, "{\"actor\":%u,\"action\":\"WAGER\",\"value\":100}",
                     dsel >= 0 ? id[dsel] : id[0]);
            send_line(svc, line);
            wait_phase(svc, &v, JV_DD_CLUE, 1000);
            check(v.phase == JV_DD_CLUE, "dd clue after wager");
            remote_do(svc, &r, &v, JV_KEY_RED, "judge dd incorrect");
            wait_phase(svc, &v, JV_REVEAL, 1000);
            check(v.phase == JV_REVEAL, "dd resolves to reveal");
            remote_do(svc, &r, &v, JV_KEY_OK, "advance dd");
            wait_phase(svc, &v, JV_BOARD, 1000);
            did_dd = 1;
            continue;
        }
        if (v.phase != JV_CLUE) continue;

        /* wait for the buzz window, then buzz player 0 */
        for (int i = 0; i < 40 && v.timer_kind != JV_TIMER_BUZZ; i++) {
            tick(svc);
            struct timespec d = {0, 5000000};
            nanosleep(&d, 0);
            sync_view(&v);
        }
        char line[96];
        snprintf(line, sizeof line, "{\"actor\":%u,\"action\":\"BUZZ\"}", id[0]);
        send_line(svc, line);
        sync_view(&v);
        check(v.buzzed >= 0, "buzzed slot recorded (signed)");

        int correct = !did_correct;
        remote_do(svc, &r, &v, correct ? JV_KEY_OK : JV_KEY_RED, correct ? "judge correct" : "judge incorrect");
        wait_phase(svc, &v, JV_REVEAL, 1000);
        if (v.phase != JV_REVEAL) {
            host_reveal(svc, &r, &v);       /* an incorrect answer can reopen the buzz */
            wait_phase(svc, &v, JV_REVEAL, 1000);
        }
        check(v.phase == JV_REVEAL, "clue resolves to reveal");
        remote_do(svc, &r, &v, JV_KEY_OK, "advance clue");
        wait_phase(svc, &v, JV_BOARD, 1000);
        if (correct) did_correct = 1; else did_incorrect = 1;
    }
    check(did_correct && did_incorrect && did_dd, "normal correct, incorrect and daily double exercised");

    /* pause/resume through the remote overlay */
    wait_phase(svc, &v, JV_BOARD, 1000);
    jpq_remote_init(&r);
    remote_do(svc, &r, &v, JV_KEY_BACK, "pause");
    sync_view(&v);
    check(v.paused == 1, "game paused via remote");
    remote_do(svc, &r, &v, JV_KEY_OK, "resume");
    sync_view(&v);
    check(v.paused == 0, "game resumed via remote");
    check(r.col >= 0 && r.col < JPQ_VIEW_COLS, "focus tracked locally");

    /* Leaving the pause menu with BACK keeps the game paused on the overlay,
     * where OK is the explicit resume, so the operator is never stranded. */
    jpq_remote_init(&r);
    remote_do(svc, &r, &v, JV_KEY_BACK, "pause again");
    sync_view(&v);
    check(v.paused == 1 && r.menu == JRM_PAUSE, "pause menu open");
    remote_do(svc, &r, &v, JV_KEY_BACK, "leave pause menu");
    sync_view(&v);
    check(v.paused == 1 && r.menu == JRM_NONE, "BACK leaves the game paused with the overlay");
    remote_do(svc, &r, &v, JV_KEY_OK, "resume from paused overlay");
    sync_view(&v);
    check(v.paused == 0, "paused overlay OK resumes");

    /* Real paused host cancellation: arm a restart from the pause menu, prove a
     * player cannot cancel the host confirmation, cancel with BACK, then commit
     * the restart (host-only, allowed while frozen). */
    jpq_remote_init(&r);
    remote_do(svc, &r, &v, JV_KEY_BACK, "pause for restart");
    sync_view(&v);
    check(v.paused == 1, "paused before restart");
    r.cursor = 1;                                  /* pause menu RESTART */
    remote_do(svc, &r, &v, JV_KEY_OK, "arm restart while paused");
    sync_view(&v);
    check(v.restart_confirm == 1 && v.paused == 1, "restart armed while paused");
    {
        char line[128];
        snprintf(line, sizeof line, "{\"actor\":%u,\"action\":\"CANCEL\"}", id[0]);
        send_line(svc, line);
        sync_view(&v);
        check(v.restart_confirm == 1, "player cannot cancel a host confirmation while paused");
    }
    remote_do(svc, &r, &v, JV_KEY_BACK, "cancel restart while paused");
    sync_view(&v);
    check(v.restart_confirm == 0 && v.paused == 1, "host CANCEL while paused keeps the game paused");
    /* re-enter through INFO (BACK now resumes) and commit the paused restart */
    jpq_remote_init(&r);
    remote_do(svc, &r, &v, JV_KEY_INFO, "settings while paused");
    r.cursor = 1;
    remote_do(svc, &r, &v, JV_KEY_OK, "arm restart via settings while paused");
    sync_view(&v);
    check(v.restart_confirm == 1, "restart armed via settings while paused");
    remote_do(svc, &r, &v, JV_KEY_OK, "commit restart while paused");
    wait_phase(svc, &v, JV_LOBBY, 1000);
    check(v.phase == JV_LOBBY && v.paused == 0, "restart while paused returns to lobby");

    /* restart is a two-phase host confirmation */
    jpq_remote_init(&r);
    r.menu = JRM_SETTINGS;
    r.cursor = 1;
    remote_do(svc, &r, &v, JV_KEY_OK, "arm restart");
    sync_view(&v);
    check(v.restart_confirm == 1, "restart armed in core");
    remote_do(svc, &r, &v, JV_KEY_OK, "commit restart");
    wait_phase(svc, &v, JV_LOBBY, 1000);
    check(v.phase == JV_LOBBY, "restart returns to lobby");

    /* installed-run gate: ready everyone, then prove gameplay is refused while
     * the native surface is inactive and accepted once it is active. */
    for (int i = 0; i < 3; i++) {
        char line[96];
        snprintf(line, sizeof line, "{\"actor\":%u,\"action\":\"READY\",\"value\":1}", id[i]);
        send_line(svc, line);
    }
    sync_view(&v);
    jpq_service_set_native_gate(svc, 1);
    jpq_service_set_native_active(svc, 0);
    {
        char out[512];
        JpqRemote gr;
        jpq_remote_init(&gr);
        check(jpq_remote_key(&gr, JV_KEY_OK, &v, out, sizeof out) == 1, "remote START built");
        send_line(svc, out);
        sync_view(&v);
        check(v.phase == JV_LOBBY, "gameplay rejected while native inactive");
    }
    jpq_service_set_native_active(svc, 1);
    {
        char out[512];
        JpqRemote gr;
        jpq_remote_init(&gr);
        jpq_remote_key(&gr, JV_KEY_OK, &v, out, sizeof out);
        send_line(svc, out);
        wait_phase(svc, &v, JV_BOARD, 1000);
        check(v.phase == JV_BOARD, "gameplay accepted while native active");
    }
    jpq_service_set_native_gate(svc, 0);
    jpq_service_set_native_active(svc, 0);

    /* quit is a two-phase host confirmation; cancel disarms it */
    jpq_remote_init(&r);
    remote_do(svc, &r, &v, JV_KEY_STOP, "arm quit");
    sync_view(&v);
    check(v.quit_confirm == 1 && !v.quit, "quit armed in core");
    remote_do(svc, &r, &v, JV_KEY_BACK, "cancel quit");
    sync_view(&v);
    check(v.quit_confirm == 0, "quit cancelled in core");
    jpq_remote_init(&r);
    remote_do(svc, &r, &v, JV_KEY_STOP, "arm quit again");
    remote_do(svc, &r, &v, JV_KEY_STOP, "commit quit");
    sync_view(&v);
    check(v.quit == 1, "quit committed in core");
    check(jpq_service_quit(svc) == 1, "service reports the committed quit");

    /* A committed quit does not stop the authoritative service: the native
     * relaunch reset returns to a fresh lobby on the same core, and new players
     * can still join and start without restarting the process. */
    jpq_service_request_relaunch(svc);
    tick(svc);
    check(jpq_service_relaunch_done(svc) == 1, "relaunch reset acknowledged on the poll thread");
    sync_view(&v);
    check(v.quit == 0 && v.phase == JV_LOBBY, "relaunch clears quit and returns to a fresh lobby");
    uint32_t nid = 0;
    {
        char body[128], *resp;
        snprintf(body, sizeof body, "{\"name\":\"RECON\",\"pin\":\"%s\"}", pin);
        JpqHttpRequest req;
        memset(&req, 0, sizeof req);
        strcpy(req.method, "POST");
        strcpy(req.path, "/api/join");
        req.body = body;
        req.body_len = strlen(body);
        size_t len = 0;
        const char *ct = NULL;
        int status = 200;
        resp = jpq_service_http(svc, &req, &len, &ct, &status);
        check(status == 200 && resp, "new player joins after relaunch");
        struct jval *j = resp ? json_parse(resp, strlen(resp)) : NULL;
        nid = j ? (uint32_t)jnum(jget(j, "playerId"), 0) : 0;
        jfree(j);
        free(resp);
        check(nid > 0, "relaunch join issues a player id");
    }
    for (int i = 0; i < 3; i++) {
        char line[96];
        snprintf(line, sizeof line, "{\"actor\":%u,\"action\":\"READY\",\"value\":1}", id[i]);
        send_line(svc, line);
    }
    {
        char line[96];
        snprintf(line, sizeof line, "{\"actor\":%u,\"action\":\"READY\",\"value\":1}", nid);
        send_line(svc, line);
    }
    jpq_remote_init(&r);
    {
        char out[512];
        check(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1, "remote START after relaunch");
        send_line(svc, out);
    }
    wait_phase(svc, &v, JV_BOARD, 2000);
    check(v.phase == JV_BOARD, "game starts again after relaunch without restarting the core");

    jpq_service_destroy(svc);
    char cmd[1300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir);
    if (system(cmd)) { /* best effort */ }
    printf("integration: %d checks, %d failures\n", CHECKS, FAILS);
    return FAILS ? 1 : 0;
}
