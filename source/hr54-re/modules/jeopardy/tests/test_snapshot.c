/* Native view parser vs. the authoritative public snapshot.
 *
 * Builds real games through the rules core, serialises the *actual* public
 * display the service would write for every phase with six players, and checks
 * the native parser reproduces it field for field (including signed buzz slots
 * 0/1/5/none, a signed dd_selector, redacted final answers and a clue longer
 * than the old 96-char cap). */
#define _GNU_SOURCE
#include "../src/jeopardy.h"
#include "../src/jpq_view.h"
#include "../../../reboxd/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static JpqDb db;
static JpqHistory hist;
static JpqGame g;
static uint64_t NOW;
static uint32_t SEQ;
static int checks, failures;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static int apply(uint8_t actor, uint8_t type, uint8_t target, int32_t value, const char *text) {
    JpqAction a;
    memset(&a, 0, sizeof a);
    a.seq = ++SEQ;
    a.revision = g.revision;
    a.epoch = g.epoch;
    a.actor = actor;
    a.type = type;
    a.target = target;
    a.value = value;
    if (text) snprintf(a.text, sizeof a.text, "%s", text);
    return jpq_apply(&g, &a, NOW);
}

/* Wrap like the service's write_public_display and parse with the native code,
 * then cross-check every field with the reference JSON DOM of the same bytes. */
static void assert_view_matches(void) {
    char *js = NULL;
    size_t jn = 0;
    CHECK(jpq_snapshot_json(&g, JPQ_PUBLIC, &js, &jn) == 0);
    struct sb o = {0};
    sb_puts(&o, "{\"window\":3,\"now_ms\":0,\"colors\":{\"100\":\"cobalt\"},\"state\":");
    sb_putn(&o, js, jn);
    sb_puts(&o, "}");

    JpqView v;
    CHECK(jpq_view_parse(&v, o.p, o.len) == 0);
    CHECK(v.phase == g.phase);
    CHECK(v.buzzed == (int)g.buzzed);
    CHECK(v.selector == (int)g.selector);
    CHECK(v.dd_selector == (int)(int8_t)g.dd_selector);
    CHECK(v.paused == g.paused);
    CHECK(v.player_count == g.player_count);
    CHECK(strcmp(v.players[0].color, "cobalt") == 0);

    struct jval *root = json_parse(o.p, o.len);
    struct jval *st = jget(root, "state");
    CHECK(st && st->t == J_OBJ);
    CHECK((int)jnum(jget(st, "phase"), -1) == (int)v.phase);
    CHECK((int)jnum(jget(st, "buzzed"), -99) == (int)v.buzzed);
    CHECK((int)jnum(jget(st, "dd_selector"), -99) == (int)v.dd_selector);
    CHECK((int)jnum(jget(st, "selector"), -99) == (int)v.selector);
    struct jval *players = jget(st, "players");
    CHECK(players && (int)players->n == v.player_count);
    if (players) {
        for (int i = 0; i < v.player_count && i < (int)players->n; i++) {
            struct jval *p = jnth(players, i);
            CHECK((int)jnum(jget(p, "id"), -1) == (int)v.players[i].id);
            CHECK((int)jnum(jget(p, "score"), 0) == (int)v.players[i].score);
            CHECK((jget(p, "answer")->t == J_STR) == v.players[i].has_answer);
        }
    }
    jfree(root);
    free(js);
    free(o.p);
}

static void fresh_six(void) {
    JpqConfig c;
    jpq_config_default(&c);
    c.read_ms = 10;
    jpq_init(&g, &c);
    jpq_attach(&g, &db, &hist);
    jpq_history_reset(&hist);
    for (int i = 0; i < 6; i++) {
        char n[16];
        snprintf(n, sizeof n, "P%d", i);
        jpq_add_player(&g, n, 100u + (uint32_t)i);
        jpq_set_ready(&g, (uint8_t)i, 1);
    }
    g.rng = 0x0BADF00Du;
    NOW = 1000;
    SEQ = 0;
    CHECK(apply(JPQ_HOST, JPQ_ACT_START, 0, 0, "") == 0);
    CHECK(g.player_count == 6);
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "../build/demo.jdb";
    if (jpq_db_load(&db, path)) { fprintf(stderr, "cannot load %s\n", path); return 2; }
    if (jpq_history_init(&hist, db.clue_count)) return 2;

    fresh_six();
    /* every phase the renderer branches on */
    for (int p = JPQ_PHASE_LOBBY; p <= JPQ_PHASE_REVEAL; p++) {
        g.phase = (uint8_t)p;
        g.paused = (p == JPQ_PHASE_CLUE) ? 1 : 0;
        assert_view_matches();
    }

    /* signed buzz slots: 0, 1, 5 and none must round-trip (a boolean would
     * collapse 0/none and lose 5). */
    int slots[] = {0, 1, 5, -1};
    for (size_t i = 0; i < sizeof slots / sizeof *slots; i++) {
        g.phase = JPQ_PHASE_CLUE;
        g.buzzed = (int8_t)slots[i];
        assert_view_matches();
    }

    /* signed daily-double selector: slot 0, slot 5 and none */
    int dds[] = {0, 5, 255};
    for (size_t i = 0; i < sizeof dds / sizeof *dds; i++) {
        g.phase = JPQ_PHASE_DD_WAGER;
        g.dd_selector = (uint8_t)dds[i];
        assert_view_matches();
    }

    /* a clue longer than the old 96-char cap must survive intact */
    fresh_six();
    static char longtext[301];
    for (int i = 0; i < 300; i++) longtext[i] = (char)('A' + (i % 26));
    longtext[300] = 0;
    int ci = g.board_j.clue[0][0];
    CHECK(ci >= 0);
    db.clues[ci].clue = longtext;
    db.clues[ci].clue_len = 300;
    g.act_col = 0; g.act_row = 0; g.act_value = g.board_j.value[0][0];
    g.phase = JPQ_PHASE_CLUE;
    {
        char *js = NULL; size_t jn = 0;
        CHECK(jpq_snapshot_json(&g, JPQ_PUBLIC, &js, &jn) == 0);
        struct sb o = {0};
        sb_puts(&o, "{\"window\":1,\"now_ms\":0,\"state\":");
        sb_putn(&o, js, jn);
        sb_puts(&o, "}");
        JpqView v;
        CHECK(jpq_view_parse(&v, o.p, o.len) == 0);
        CHECK(v.clue.present && v.clue.has_text);
        CHECK(strlen(v.clue.text) == 300);            /* whole clue preserved */
        CHECK(strcmp(v.clue.text, longtext) == 0);
        free(js); free(o.p);
    }

    /* public final answers are parsed per player and redaction is honoured */
    fresh_six();
    g.phase = JPQ_PHASE_FINAL_JUDGE;
    memset(g.eligible, 0, sizeof g.eligible);
    g.eligible[0] = 1; g.eligible[2] = 1;
    g.judge_cursor = 0;
    memset(g.answered, 0, sizeof g.answered);
    memset(g.judged, 0, sizeof g.judged);
    snprintf(g.answer[0], sizeof g.answer[0], "%s", "what is alpha");
    snprintf(g.answer[2], sizeof g.answer[2], "%s", "what is gamma");
    assert_view_matches();
    {
        char *js = NULL; size_t jn = 0;
        jpq_snapshot_json(&g, JPQ_PUBLIC, &js, &jn);
        struct sb o = {0};
        sb_puts(&o, "{\"window\":1,\"now_ms\":0,\"state\":");
        sb_putn(&o, js, jn);
        sb_puts(&o, "}");
        JpqView v;
        jpq_view_parse(&v, o.p, o.len);
        CHECK(v.players[0].has_answer && !strcmp(v.players[0].answer, "what is alpha"));
        CHECK(!v.players[2].has_answer);   /* not yet being judged: redacted */
        free(js); free(o.p);
    }

    /* The correct Final response is public only from FINAL_JUDGE (once every
     * private answer is in), never during wagering or answering. */
    fresh_six();
    g.round = JPQ_ROUND_FINAL;
    {
        int fi = g.board_j.clue[0][0];
        CHECK(fi >= 0 && (uint32_t)fi < db.clue_count);
        g.final_clue = fi;
    }
    {
        uint8_t phases[] = {JPQ_PHASE_FINAL_WAGER, JPQ_PHASE_FINAL_CLUE, JPQ_PHASE_FINAL_JUDGE};
        int expect_resp[] = {0, 0, 1};
        for (int i = 0; i < 3; i++) {
            g.phase = phases[i];
            char *js = NULL;
            size_t jn = 0;
            CHECK(jpq_snapshot_json(&g, JPQ_PUBLIC, &js, &jn) == 0);
            struct sb o = {0};
            sb_puts(&o, "{\"window\":1,\"now_ms\":0,\"state\":");
            sb_putn(&o, js, jn);
            sb_puts(&o, "}");
            JpqView v;
            CHECK(jpq_view_parse(&v, o.p, o.len) == 0);
            CHECK(v.final.has_text == (phases[i] != JPQ_PHASE_FINAL_WAGER));
            CHECK(v.final.has_response == expect_resp[i]);
            free(js);
            free(o.p);
        }
    }

    jpq_history_free(&hist);
    jpq_db_free(&db);
    printf("snapshot: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
