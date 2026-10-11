/* Host tests for the native renderer stack: bounded JSON, snapshot parsing,
 * scene rendering, remote key mappings, QR encode+decode, lifecycle mock child.
 * No receiver libraries, no device, no fakery. */
#include "../src/jpq_view.h"
#include "../src/jpq_render.h"
#include "../src/jpq_remote.h"
#include "../src/jpq_qr.h"
#include "../src/jpq_lifecycle.h"
#include "../src/jpq_input.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int CHECKS;

static const char *SAMPLE =
    "{\"window\":7,\"now_ms\":1000,\"colors\":{\"11\":\"cobalt\",\"22\":\"crimson\"},\"state\":{"
    "\"ok\":true,\"epoch\":3,\"revision\":42,\"phase\":1,\"paused\":0,\"round\":2,"
    "\"selector\":1,\"buzzed\":-1,\"dd_selector\":-1,\"tiebreak\":0,\"quit\":0,"
    "\"quit_confirm\":0,\"restart_confirm\":0,\"judge_cursor\":0,\"tiebreak_winner\":-1,"
    "\"timer\":{\"kind\":1,\"remaining_ms\":8500},"
    "\"config\":{\"read_ms\":5000,\"buzz_ms\":5000,\"answer_ms\":7000},"
    "\"players\":["
    "{\"id\":11,\"name\":\"Rae\",\"score\":1200,\"ready\":1,\"connected\":1,\"active\":1,\"eligible\":0,"
    "\"answered\":0,\"judged\":0,\"correct\":0,\"wager_set\":0,\"excluded\":0,\"can_buzz\":1,"
    "\"wager\":null,\"answer\":null,\"wager_min\":null,\"wager_max\":null},"
    "{\"id\":22,\"name\":\"Sam\",\"score\":-400,\"ready\":1,\"connected\":1,\"active\":1,\"eligible\":0,"
    "\"answered\":0,\"judged\":0,\"correct\":0,\"wager_set\":0,\"excluded\":0,\"can_buzz\":0,"
    "\"wager\":null,\"answer\":null,\"wager_min\":null,\"wager_max\":null}],"
    "\"board\":{\"round\":2,\"categories\":["
    "{\"name\":\"BEFORE & AFTER\",\"clues\":["
    "{\"value\":200,\"used\":0,\"daily_double\":0},{\"value\":400,\"used\":0,\"daily_double\":0},"
    "{\"value\":600,\"used\":1,\"daily_double\":0},{\"value\":800,\"used\":0,\"daily_double\":0},"
    "{\"value\":1000,\"used\":0,\"daily_double\":0}]},"
    "{\"name\":\"SCIENCE\",\"clues\":["
    "{\"value\":200,\"used\":0,\"daily_double\":0},{\"value\":400,\"used\":0,\"daily_double\":0},"
    "{\"value\":600,\"used\":0,\"daily_double\":0},{\"value\":800,\"used\":0,\"daily_double\":0},"
    "{\"value\":1000,\"used\":0,\"daily_double\":0}]},"
    "{\"name\":\"MOVIES\",\"clues\":["
    "{\"value\":200,\"used\":0,\"daily_double\":0},{\"value\":400,\"used\":0,\"daily_double\":0},"
    "{\"value\":600,\"used\":0,\"daily_double\":0},{\"value\":800,\"used\":0,\"daily_double\":0},"
    "{\"value\":1000,\"used\":0,\"daily_double\":0}]},"
    "{\"name\":\"HISTORY\",\"clues\":["
    "{\"value\":200,\"used\":0,\"daily_double\":0},{\"value\":400,\"used\":0,\"daily_double\":0},"
    "{\"value\":600,\"used\":0,\"daily_double\":0},{\"value\":800,\"used\":0,\"daily_double\":0},"
    "{\"value\":1000,\"used\":0,\"daily_double\":0}]},"
    "{\"name\":\"LITERATURE\",\"clues\":["
    "{\"value\":200,\"used\":0,\"daily_double\":0},{\"value\":400,\"used\":0,\"daily_double\":0},"
    "{\"value\":600,\"used\":0,\"daily_double\":0},{\"value\":800,\"used\":0,\"daily_double\":0},"
    "{\"value\":1000,\"used\":0,\"daily_double\":0}]},"
    "{\"name\":\"POTPOURRI\",\"clues\":["
    "{\"value\":200,\"used\":0,\"daily_double\":0},{\"value\":400,\"used\":0,\"daily_double\":0},"
    "{\"value\":600,\"used\":0,\"daily_double\":0},{\"value\":800,\"used\":0,\"daily_double\":0},"
    "{\"value\":1000,\"used\":0,\"daily_double\":0}]}]},"
    "\"clue\":{\"col\":2,\"row\":1,\"value\":400,\"daily_double\":0,\"category\":\"SCIENCE\","
    "\"text\":\"This gas makes up about 21 percent of Earth's atmosphere\",\"response\":null},"
    "\"final\":null,\"results\":{\"leader\":0,\"tie\":0}}}";

static const char *META =
    "{\"version\":1,\"service\":\"jeopardy\",\"protocol\":1,"
    "\"metadataUrl\":\"http://192.168.1.20:8135/\",\"joinPin\":\"4821\",\"hostPin\":\"654321\","
    "\"publicDisplay\":\"/data/native/public-display.json\",\"commandSocket\":\"/data/native/command.sock\","
    "\"port\":8135}";

static void test_parse(void) {
    JpqView v;
    assert(jpq_view_parse(&v, SAMPLE, strlen(SAMPLE)) == 0);
    CHECKS++;
    assert(v.valid && v.window == 7 && v.now_ms == 1000);
    assert(v.phase == JV_BOARD && v.round == JV_ROUND_DJ && v.revision == 42);
    assert(v.player_count == 2 && v.players[0].id == 11 && v.players[0].score == 1200);
    assert(v.players[1].score == -400 && v.players[1].can_buzz == 0);
    assert(strcmp(v.categories[0].name, "BEFORE & AFTER") == 0);
    assert(v.categories[0].used[2] == 1 && v.categories[0].value[4] == 1000);
    assert(v.clue.present && v.clue.col == 2 && v.clue.row == 1 && v.clue.value == 400);
    assert(v.clue.has_text && !v.clue.has_response);
    assert(v.cfg_read_ms == 5000 && v.cfg_answer_ms == 7000);
    assert(v.buzzed == -1 && v.dd_selector == -1);            /* signed slots */
    assert(strcmp(v.players[0].color, "cobalt") == 0);        /* service colour */
    assert(strcmp(v.players[1].color, "crimson") == 0);
    assert(!v.players[0].has_answer);                         /* redacted answer */
    CHECKS++;
    JpqMetadata m;
    assert(jpq_metadata_parse(&m, META, strlen(META)) == 0);
    assert(strcmp(m.url, "http://192.168.1.20:8135/") == 0 && strcmp(m.join_pin, "4821") == 0);
    assert(strcmp(m.host_pin, "654321") == 0 && m.port == 8135);
    CHECKS++;
    /* malformed and duplicate-key inputs are rejected */
    assert(jpq_view_parse(&v, "{\"a\":1}", 7) != 0);
    assert(jpq_view_parse(&v, "{\"window\":1,\"window\":2}", 23) != 0);
    CHECKS++;
}

static int nonblank(UiFramebuffer *f) {
    int distinct = 0;
    for (int i = 0; i < 64; i++) {
        const uint8_t *p = f->pixels + (size_t)(i * 977) * 4;
        if (p[0] || p[1] || p[2]) distinct++;
    }
    return distinct;
}

static void test_render(void) {
    UiFramebuffer f;
    assert(!ui_fb_init(&f, 720, 480));
    JpqView v;
    assert(jpq_view_parse(&v, SAMPLE, strlen(SAMPLE)) == 0);
    JpqMetadata m;
    jpq_metadata_parse(&m, META, strlen(META));
    uint8_t phases[] = {JV_LOBBY, JV_BOARD, JV_CLUE, JV_DD_WAGER, JV_DD_CLUE,
                        JV_FINAL_WAGER, JV_FINAL_CLUE, JV_FINAL_JUDGE, JV_RESULTS, JV_TIEBREAK_CLUE, JV_REVEAL};
    for (size_t i = 0; i < sizeof phases; i++) {
        v.phase = phases[i];
        jpq_render(&f, &v, &m, 5000, 0, 0);
        assert(nonblank(&f) > 10);
        CHECKS++;
    }
    /* an all-zero view renders the waiting card without crashing */
    JpqView empty;
    memset(&empty, 0, sizeof empty);
    jpq_render(&f, &empty, &m, 0, 0, 0);
    assert(nonblank(&f) > 5);
    CHECKS++;
    /* remote overlays render */
    JpqRemote r;
    jpq_remote_init(&r);
    r.menu = JRM_SETTINGS;
    jpq_render_overlay(&f, &r, &m, &v);
    r.menu = JRM_HOST;
    jpq_render_overlay(&f, &r, &m, &v);
    r.menu = JRM_TIMERS;
    jpq_render_overlay(&f, &r, &m, &v);
    CHECKS++;
    ui_fb_free(&f);
}

static void test_remote(void) {
    JpqView v;
    jpq_view_parse(&v, SAMPLE, strlen(SAMPLE));
    JpqRemote r;
    char out[512];
    jpq_remote_init(&r);
    /* lobby: OK starts */
    v.phase = JV_LOBBY;
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1);
    assert(strstr(out, "\"action\":\"START\"") && strstr(out, "\"actor\":\"host\""));
    /* board: focus moves locally then OK selects col*5+row */
    v.phase = JV_BOARD;
    jpq_remote_init(&r);
    assert(jpq_remote_key(&r, JV_KEY_RIGHT, &v, out, sizeof out) == 0 && r.col == 1 && r.row == 0);
    assert(jpq_remote_key(&r, JV_KEY_DOWN, &v, out, sizeof out) == 0 && r.row == 1);
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1);
    assert(strstr(out, "\"action\":\"SELECT\"") && strstr(out, "\"target\":6"));
    CHECKS++;

    /* clue with slot 5 buzzing: OK judges the *buzzed* player correct, RED
     * incorrect, never the board selector. Slot 0 and slot 1 prove the signed
     * slot is used, not a boolean. */
    v.phase = JV_CLUE;
    v.selector = 0;
    v.buzzed = 1;
    jpq_remote_init(&r);
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1);
    assert(strstr(out, "JUDGE") && strstr(out, "\"target\":22") && strstr(out, "\"value\":1"));
    assert(jpq_remote_key(&r, JV_KEY_RED, &v, out, sizeof out) == 1);
    assert(strstr(out, "JUDGE") && strstr(out, "\"target\":22") && strstr(out, "\"value\":-1"));
    /* no buzzer yet: OK does nothing here (reveal/pass are in the host menu) */
    v.buzzed = -1;
    jpq_remote_init(&r);
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 0);
    CHECKS++;

    /* daily double judges the dd_selector id, not the board selector */
    v.phase = JV_DD_CLUE;
    v.selector = 1;
    v.dd_selector = 0;
    jpq_remote_init(&r);
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1);
    assert(strstr(out, "JUDGE") && strstr(out, "\"target\":11") && strstr(out, "\"value\":1"));
    assert(jpq_remote_key(&r, JV_KEY_RED, &v, out, sizeof out) == 1);
    assert(strstr(out, "JUDGE") && strstr(out, "\"target\":11") && strstr(out, "\"value\":-1"));
    v.phase = JV_DD_WAGER;
    jpq_remote_init(&r);
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1 && strstr(out, "ADVANCE"));
    CHECKS++;

    /* reveal/results advance; final judge uses OK=correct, RED=incorrect */
    v.phase = JV_REVEAL;
    jpq_remote_init(&r);
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1 && strstr(out, "ADVANCE"));
    v.phase = JV_FINAL_JUDGE;
    v.judge_cursor = 1;
    jpq_remote_init(&r);
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1 && strstr(out, "\"value\":1"));
    assert(jpq_remote_key(&r, JV_KEY_RED, &v, out, sizeof out) == 1 && strstr(out, "\"value\":-1"));
    CHECKS++;

    /* BACK pauses and opens the menu */
    v.phase = JV_BOARD;
    jpq_remote_init(&r);
    assert(jpq_remote_key(&r, JV_KEY_BACK, &v, out, sizeof out) == 1 && strstr(out, "PAUSE") && r.menu == JRM_PAUSE);
    assert(jpq_remote_key(&r, JV_KEY_BACK, &v, out, sizeof out) == 0 && r.menu == JRM_NONE);

    /* STOP is a real two-phase core confirmation: first press arms (sends
     * QUIT), second commits (sends QUIT again); BACK sends CANCEL. */
    jpq_remote_init(&r);
    assert(jpq_remote_key(&r, JV_KEY_STOP, &v, out, sizeof out) == 1 && strstr(out, "QUIT") && r.menu == JRM_CONFIRM_QUIT);
    assert(jpq_remote_key(&r, JV_KEY_BACK, &v, out, sizeof out) == 1 && strstr(out, "CANCEL"));
    jpq_remote_init(&r);
    assert(jpq_remote_key(&r, JV_KEY_STOP, &v, out, sizeof out) == 1);
    assert(jpq_remote_key(&r, JV_KEY_STOP, &v, out, sizeof out) == 1 && strstr(out, "QUIT") && r.menu == JRM_NONE);
    CHECKS++;

    /* INFO opens settings; host PIN and timer edit reach CONFIG */
    jpq_remote_init(&r);
    assert(jpq_remote_key(&r, JV_KEY_INFO, &v, out, sizeof out) == 0 && r.menu == JRM_SETTINGS);
    r.cursor = 5;
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 0 && r.menu == JRM_TIMERS);
    assert(r.t_read == 5000 && r.t_answer == 7000);
    assert(jpq_remote_key(&r, JV_KEY_RIGHT, &v, out, sizeof out) == 0 && r.t_read == 6000);
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1);
    assert(strstr(out, "CONFIG") && strstr(out, "\"read_ms\":6000"));
    /* score edit reaches SCORE with the player id */
    jpq_remote_init(&r); r.menu = JRM_SETTINGS; r.cursor = 3;
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 0 && r.menu == JRM_SCORE);
    assert(r.score_value == 1200);
    assert(jpq_remote_key(&r, JV_KEY_RIGHT, &v, out, sizeof out) == 0 && r.score_value == 1300);
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1);
    assert(strstr(out, "SCORE") && strstr(out, "\"target\":11") && strstr(out, "\"value\":1300"));

    /* RESTART from settings arms the core then commits; cancel disarms */
    jpq_remote_init(&r); r.menu = JRM_SETTINGS; r.cursor = 1;
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1 && strstr(out, "RESTART") && r.menu == JRM_CONFIRM_RESTART);
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1 && strstr(out, "RESTART"));
    jpq_remote_init(&r); r.menu = JRM_SETTINGS; r.cursor = 1;
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1);
    assert(jpq_remote_key(&r, JV_KEY_BACK, &v, out, sizeof out) == 1 && strstr(out, "CANCEL"));
    /* settings exposes reveal/pass in the host menu */
    jpq_remote_init(&r); r.menu = JRM_SETTINGS; r.cursor = 6;
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1 && strstr(out, "REVEAL"));
    v.buzzed = 1;
    jpq_remote_init(&r); r.menu = JRM_SETTINGS; r.cursor = 7;
    assert(jpq_remote_key(&r, JV_KEY_OK, &v, out, sizeof out) == 1 && strstr(out, "PASS") && strstr(out, "\"target\":22"));
    CHECKS++;
    /* key translate matches the broker map */
    assert(jpq_key_translate(0xe100) == JV_KEY_UP);
    assert(jpq_key_translate(0xe103) == JV_KEY_RIGHT);
    assert(jpq_key_translate(0xe001) == JV_KEY_OK);
    assert(jpq_key_translate(0x1e402) == JV_KEY_STOP);
    assert(jpq_key_translate(0xe200) == JV_KEY_RED);
    assert(jpq_key_translate(0xe502) == JV_KEY_NONE);
    CHECKS++;
}

/* A real-shaped public display with the fields the defect fix relies on:
 * six players, a signed buzz slot 5, a signed dd_selector 0, a submitted final
 * answer and a clue longer than the old 96-char cap. */
static void test_signed_long(void) {
    static char doc[16384];
    char longtext[320];
    for (int i = 0; i < 300; i++) longtext[i] = (char)('A' + (i % 26));
    longtext[300] = 0;
    int cap = (int)sizeof doc, off = 0;
    off += snprintf(doc + off, cap - off,
        "{\"window\":9,\"now_ms\":2000,\"colors\":{\"1\":\"jade\"},\"state\":{"
        "\"ok\":true,\"epoch\":4,\"revision\":9,\"phase\":7,\"paused\":0,\"round\":3,"
        "\"selector\":2,\"buzzed\":5,\"dd_selector\":0,\"tiebreak\":0,\"quit\":0,"
        "\"quit_confirm\":0,\"restart_confirm\":0,\"judge_cursor\":5,\"tiebreak_winner\":-1,"
        "\"timer\":{\"kind\":0,\"remaining_ms\":0},"
        "\"config\":{\"read_ms\":1,\"buzz_ms\":1,\"answer_ms\":1},\"players\":[");
    for (int i = 0; i < 6; i++) {
        off += snprintf(doc + off, cap - off,
            "%s{\"id\":%d,\"name\":\"P%d\",\"score\":%d,\"ready\":1,\"connected\":1,\"active\":1,"
            "\"eligible\":1,\"answered\":%d,\"judged\":%d,\"correct\":0,\"wager_set\":1,"
            "\"excluded\":0,\"can_buzz\":0,\"wager\":100,\"answer\":\"My final answer %d\","
            "\"wager_min\":null,\"wager_max\":null}",
            i ? "," : "", i + 1, i + 1, i * 100, i == 0 ? 1 : 0, i == 0 ? 0 : 1, i);
    }
    off += snprintf(doc + off, cap - off,
        "],\"board\":{\"round\":3,\"categories\":[]},\"clue\":null,"
        "\"final\":{\"category\":\"WORLD CAPITALS OF EUROPE\",\"text\":\"%s\","
        "\"response\":\"What is a deliberately long correct response that must wrap?\"},"
        "\"results\":{\"leader\":0,\"tie\":0}}}", longtext);
    assert(off < cap);

    JpqView v;
    assert(jpq_view_parse(&v, doc, strlen(doc)) == 0);
    assert(v.player_count == 6);
    assert(v.buzzed == 5 && v.dd_selector == 0 && v.selector == 2);
    assert(strcmp(v.players[0].color, "jade") == 0);
    assert(v.players[0].has_answer && strcmp(v.players[0].answer, "My final answer 0") == 0);
    assert(v.players[0].judged == 0 && v.players[1].judged == 1);
    assert(strlen(v.final.text) == 300);          /* whole clue, not truncated */
    assert(v.final.has_response && strlen(v.final.response) > 40);
    CHECKS++;

    /* the remote targets the real signed slots: buzz slot 5 -> player id 6,
     * dd_selector 0 -> player id 1. */
    JpqRemote rr;
    char cmd[512];
    v.phase = JV_TIEBREAK_CLUE;
    jpq_remote_init(&rr);
    assert(jpq_remote_key(&rr, JV_KEY_OK, &v, cmd, sizeof cmd) == 1);
    assert(strstr(cmd, "JUDGE") && strstr(cmd, "\"target\":6") && strstr(cmd, "\"value\":1"));
    v.phase = JV_DD_CLUE;
    jpq_remote_init(&rr);
    assert(jpq_remote_key(&rr, JV_KEY_RED, &v, cmd, sizeof cmd) == 1);
    assert(strstr(cmd, "JUDGE") && strstr(cmd, "\"target\":1") && strstr(cmd, "\"value\":-1"));
    CHECKS++;

    /* every play scene renders with 6 players and an over-long clue without
     * crashing (bounded wrap keeps it inside the safe margins). */
    UiFramebuffer f;
    assert(!ui_fb_init(&f, 720, 480));
    JpqMetadata m;
    jpq_metadata_parse(&m, META, strlen(META));
    uint8_t phases[] = {JV_BOARD, JV_CLUE, JV_FINAL_JUDGE, JV_RESULTS};
    for (size_t i = 0; i < sizeof phases; i++) {
        v.phase = phases[i];
        jpq_render(&f, &v, &m, 6000, 2000, 0);
        assert(nonblank(&f) > 10);
        CHECKS++;
    }
    ui_fb_free(&f);
}

/* ---- QR self-check decoder (versions 1..4, ECC L, single block) -------- */
static int qr_decode(const JpqQr *q, char *out, size_t cap) {
    int size = q->size;
    static uint8_t func[JPQ_QR_MAX][JPQ_QR_MAX];
    memset(func, 0, sizeof func);
    for (int c = 0; c < 3; c++) {
        int r0 = (c == 2) ? size - 7 : 0;
        int c0 = (c == 1) ? size - 7 : 0;
        for (int r = -1; r <= 7; r++) for (int cc = -1; cc <= 7; cc++) {
            int rr = r0 + r, cx = c0 + cc;
            if (rr >= 0 && cx >= 0 && rr < size && cx < size) func[rr][cx] = 1;
        }
    }
    for (int i = 8; i <= size - 9; i++) { func[6][i] = 1; func[i][6] = 1; }
    int aligns[2][2] = {{6, 18}, {6, 22}};
    int na = q->version >= 2 ? 2 : 0;
    int centers[2] = {0, 0};
    if (q->version == 2) { centers[0] = 6; centers[1] = 18; }
    if (q->version == 3) { centers[0] = 6; centers[1] = 22; }
    if (q->version == 4) { centers[0] = 6; centers[1] = 26; }
    (void)aligns;
    for (int a = 0; a < na; a++) for (int b = 0; b < na; b++) {
        int ar = centers[a], ac = centers[b];
        if ((ar <= 8 && ac <= 8) || (ar <= 8 && ac >= size - 9) || (ar >= size - 9 && ac <= 8)) continue;
        for (int r = -2; r <= 2; r++) for (int c = -2; c <= 2; c++) func[ar + r][ac + c] = 1;
    }
    func[size - 8][8] = 1;
    for (int i = 0; i <= 8; i++) { func[8][i] = 1; func[i][8] = 1; }
    for (int i = 0; i < 8; i++) { func[8][size - 1 - i] = 1; func[size - 1 - i][8] = 1; }
    /* read format bits */
    int fmt = 0;
    for (int i = 0; i <= 5; i++) fmt |= q->module[i][8] << i;
    fmt |= q->module[7][8] << 6;
    fmt |= q->module[8][8] << 7;
    fmt |= q->module[8][7] << 8;
    for (int i = 9; i < 15; i++) fmt |= q->module[8][14 - i] << i;
    fmt ^= 0x5412;
    int mask = (fmt >> 10) & 7;
    if (((fmt >> 13) & 3) != 1) return -1; /* must be ECC L */
    /* collect data bits in placement order, unmasked */
    static uint8_t bits[4096];
    int nbits = 0, up = 1;
    for (int col = size - 1; col > 0; col -= 2) {
        if (col == 6) col--;
        for (int i = 0; i < size; i++) {
            int row = up ? size - 1 - i : i;
            for (int c = 0; c < 2; c++) {
                int cc = col - c;
                if (func[row][cc]) continue;
                int v = q->module[row][cc];
                int mb;
                switch (mask) {
                case 0: mb = (row + cc) % 2 == 0; break;
                case 1: mb = row % 2 == 0; break;
                case 2: mb = cc % 3 == 0; break;
                case 3: mb = (row + cc) % 3 == 0; break;
                case 4: mb = (row / 2 + cc / 3) % 2 == 0; break;
                case 5: mb = (row * cc) % 2 + (row * cc) % 3 == 0; break;
                case 6: mb = ((row * cc) % 2 + (row * cc) % 3) % 2 == 0; break;
                default: mb = ((row + cc) % 2 + (row * cc) % 3) % 2 == 0; break;
                }
                if (mb) v ^= 1;
                if (nbits < (int)sizeof bits) bits[nbits++] = (uint8_t)v;
            }
        }
        up = !up;
    }
    if (nbits < 12) return -1;
    int mode = (bits[0] << 3) | (bits[1] << 2) | (bits[2] << 1) | bits[3];
    if (mode != 4) return -1; /* byte mode */
    int count = 0;
    for (int i = 4; i < 12; i++) count = (count << 1) | bits[i];
    if (count < 0 || (size_t)count + 1 > cap) return -1;
    int bit = 12;
    for (int i = 0; i < count; i++) {
        int b = 0;
        for (int k = 0; k < 8; k++) b = (b << 1) | bits[bit++];
        out[i] = (char)b;
    }
    out[count] = 0;
    return count;
}

static void test_qr(void) {
    const char *url = "http://192.168.1.20:8135/#pin=4821";
    JpqQr q;
    assert(jpq_qr_encode(url, -1, &q) == 0);
    char dec[128];
    assert(qr_decode(&q, dec, sizeof dec) == (int)strlen(url));
    assert(strcmp(dec, url) == 0);
    CHECKS++;
    /* every version and forced mask round-trips through the decoder */
    const char *cases[] = {"http://a.co/x#7", "http://192.168.1.20:8135/#pin=4821",
                           "http://192.168.100.200:8135/join?pin=4821",
                           "http://10.0.0.123:8135/#host-pin-654321-abcdefghijklmnop"};
    for (int i = 0; i < 4; i++) {
        for (int m = 0; m < 8; m++) {
            assert(jpq_qr_encode(cases[i], m, &q) == 0);
            assert(q.mask == m);
            assert(qr_decode(&q, dec, sizeof dec) == (int)strlen(cases[i]));
            assert(strcmp(dec, cases[i]) == 0);
        }
        CHECKS++;
    }
    /* known reference matrix (python `qrcode`, version 2, ECC L, mask 0) */
    JpqQr ref;
    assert(jpq_qr_encode("http://192.168.1.20:8135/#1234", 0, &ref) == 0);
    static const char *golden[25] = {
        "#######....##..##.#######", "#.....#..####..#..#.....#", "#.###.#.#.###.##..#.###.#",
        "#.###.#..#.##.....#.###.#", "#.###.#..####..##.#.###.#", "#.....#...##.#.##.#.....#",
        "#######.#.#.#.#.#.#######", "........#....#.#.........", "###.#####....##..##...#..",
        "######..##....#...##....#", ".##.#.##..#..##.#.###.###", "###.##..####.#..#####..#.",
        ".#..#.###.#..####.##.#.##", "....#.....#...#..###.#..#", "#.##.####.....#.##.##.###",
        ".#####.#.....#.#.###.#.#.", "#.....###...##########...", "........###..##.#...#####",
        "#######.##....###.#.#..##", "#.....#.##..#####...##.##", "#.###.#.#######.#####..#.",
        "#.###.#..##..#####..#.#..", "#.###.#.###..#####..##..#", "#.....#.#..####.##..##.#.",
        "#######.#....##.####...##"};
    for (int r = 0; r < ref.size; r++) {
        char row[JPQ_QR_MAX + 1];
        for (int c = 0; c < ref.size; c++) row[c] = ref.module[r][c] ? '#' : '.';
        row[ref.size] = 0;
        assert(strcmp(row, golden[r]) == 0);
    }
    CHECKS++;
    /* the QR payload appends the join PIN as a fragment */
    JpqMetadata m;
    jpq_metadata_parse(&m, META, strlen(META));
    char payload[300];
    jpq_qr_payload(payload, sizeof payload, &m);
    assert(strcmp(payload, "http://192.168.1.20:8135/#pin=4821") == 0);
    JpqQr pq;
    assert(jpq_qr_encode(payload, -1, &pq) == 0);
    assert(qr_decode(&pq, dec, sizeof dec) == (int)strlen(payload));
    assert(strcmp(dec, payload) == 0);
    CHECKS++;
}

static void test_lifecycle(void) {
    char dir[] = "/tmp/jpq-life-XXXXXX";
    assert(mkdtemp(dir));
    char engine[512], pidp[512], lockp[512];
    snprintf(engine, sizeof engine, "%s/mock-engine", dir);
    snprintf(pidp, sizeof pidp, "%s/native.pid", dir);
    snprintf(lockp, sizeof lockp, "%s/native.lock", dir);
    /* copy the compiled mock engine (path from JPQ_MOCK_ENGINE) */
    char src[512];
    const char *mock = getenv("JPQ_MOCK_ENGINE");
    snprintf(src, sizeof src, "%s", mock ? mock : "build/mock-engine");
    FILE *in = fopen(src, "rb");
    if (!in) { snprintf(src, sizeof src, "../build/mock-engine"); in = fopen(src, "rb"); }
    assert(in);
    FILE *out = fopen(engine, "wb");
    assert(out);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    chmod(engine, 0700);

    int pid = 0;
    char err[160];
    assert(jpq_lifecycle_start(engine, pidp, lockp, "2000", 5000, &pid, err, sizeof err) == 0);
    assert(pid > 1);
    assert(jpq_lifecycle_pid(engine, pidp) == pid);   /* pid identity via /proc */
    assert(jpq_lifecycle_busy(lockp) == 1);           /* lock retained while running */
    /* a foreign engine path must not be mistaken for ours */
    assert(jpq_lifecycle_pid("/bin/sh", pidp) == -1);
    CHECKS++;
    int was = 0;
    assert(jpq_lifecycle_stop(engine, pidp, lockp, &was) == 0);
    assert(was == 1);
    assert(jpq_lifecycle_busy(lockp) == 0);           /* ownership returned */
    assert(access(pidp, F_OK) != 0);
    CHECKS++;
    /* stopping again is a no-op */
    assert(jpq_lifecycle_stop(engine, pidp, lockp, &was) == 0 && was == 0);
    unlink(engine);
    rmdir(dir);
    CHECKS++;
}

/* Build a polished six-player Double Jeopardy view with a long category, a
 * long clue, a submitted final answer and a colour per podium. */
static void preview_build(JpqView *v) {
    memset(v, 0, sizeof *v);
    v->valid = 1;
    v->window = 1;
    v->round = JV_ROUND_DJ;
    v->phase = JV_BOARD;
    v->player_count = 6;
    const char *names[6] = {"RAE", "SAM", "JORDAN", "ALEX", "CASEY", "MORGAN"};
    const char *cols[6] = {"cobalt", "gold", "crimson", "jade", "violet", "amber"};
    for (int i = 0; i < 6; i++) {
        JvPlayer *p = &v->players[i];
        p->id = (uint32_t)(11 + i);
        snprintf(p->name, sizeof p->name, "%s", names[i]);
        p->score = (i + 1) * 400 - (i == 2 ? 800 : 0);
        p->ready = p->connected = p->active = 1;
        snprintf(p->color, sizeof p->color, "%s", cols[i]);
    }
    const char *cats[6] = {"BEFORE & AFTER", "SCIENCE", "INTERNATIONAL RELATIONS",
                           "HISTORY", "LITERATURE", "POTPOURRI"};
    const int vals[5] = {400, 800, 1200, 1600, 2000};
    for (int c = 0; c < JPQ_VIEW_COLS; c++) {
        snprintf(v->categories[c].name, sizeof v->categories[c].name, "%s", cats[c]);
        for (int r = 0; r < JPQ_VIEW_ROWS; r++) {
            v->categories[c].value[r] = vals[r];
            v->categories[c].used[r] = (c == 0 && r < 2) ? 1 : 0;
        }
    }
    /* a real long clue so wrapping and adaptive fonts are exercised */
    v->clue.present = 1;
    v->clue.has_text = 1;
    v->clue.has_response = 1;
    v->clue.col = 2; v->clue.row = 1; v->clue.value = 800;
    snprintf(v->clue.category, sizeof v->clue.category, "%s", "INTERNATIONAL RELATIONS");
    snprintf(v->clue.text, sizeof v->clue.text, "%s",
        "After a 1995 referendum this Nordic nation stayed out of the European Union, "
        "keeping its own currency rather than adopting the euro");
    snprintf(v->clue.response, sizeof v->clue.response, "%s", "What is Norway?");
    v->buzzed = 3;
    v->selector = 2;
    v->dd_selector = 3;
    v->final.present = 1;
    v->final.has_text = 1;
    v->final.has_response = 0;   /* public never sees it while judging */
    snprintf(v->final.category, sizeof v->final.category, "%s", "WORLD CAPITALS");
    snprintf(v->final.text, sizeof v->final.text, "%s",
        "This city on the Seine hosted two World's Fairs and is home to the Louvre");
    snprintf(v->final.response, sizeof v->final.response, "%s", "What is Paris?");
    snprintf(v->players[2].answer, sizeof v->players[2].answer, "%s", "what is lyon");
    v->players[2].has_answer = 1;
    v->players[2].eligible = 1; v->players[2].judged = 0; v->players[2].correct = 0; v->players[2].wager = 800; v->players[2].wager_set = 1;
    v->players[0].eligible = 1; v->players[0].judged = 1; v->players[0].correct = 1; v->players[0].wager = 400; v->players[0].wager_set = 1;
    v->players[3].eligible = 1; v->players[3].judged = 1; v->players[3].correct = 0; v->players[3].wager = 1200; v->players[3].wager_set = 1;
    v->players[5].eligible = 1;
    v->judge_cursor = 2;
    v->leader = 5;
    v->cfg_read_ms = 5000; v->cfg_buzz_ms = 5000; v->cfg_answer_ms = 8000;
    v->timer_kind = JV_TIMER_NONE;
}

static int pixel_is(UiFramebuffer *f, int x, int y, int r, int g, int b) {
    if (x < 0 || y < 0 || x >= f->width || y >= f->height) return 0;
    const uint8_t *p = f->pixels + ((size_t)y * f->width + x) * 4;
    return p[0] == r && p[1] == g && p[2] == b;
}

static int pixel_has_green_win(UiFramebuffer *f) {
    for (int y = 92; y < 136; y++)
        for (int x = 0; x < f->width; x++) {
            const uint8_t *p = f->pixels + ((size_t)y * f->width + x) * 4;
            if (p[1] > 240 && p[0] > 130 && p[0] < 175 && p[2] > 170 && p[2] < 215) return 1;
        }
    return 0;
}

/* The TV faces the preview judge: six podiums, a whole >96-char clue, the
 * buzzed (not leading) podium highlighted during a clue, a bright bordered
 * winner name and the Final adjudication reference answer. */
static void test_preview_layout(void) {
    UiFramebuffer f;
    assert(!ui_fb_init(&f, 720, 480));
    JpqMetadata m;
    jpq_metadata_parse(&m, META, strlen(META));

    JpqView v;
    preview_build(&v);
    assert(v.player_count == 6);
    for (int i = 0; i < 6; i++) assert(v.players[i].name[0] && v.players[i].color[0]);
    assert(strlen(v.clue.text) > 96);                 /* long clue, whole */
    assert(v.final.category[0] && v.players[2].has_answer);
    CHECKS++;

    /* the podium highlight follows the buzzed player, never the leader */
    v.phase = JV_CLUE;
    assert(v.buzzed == 3 && v.leader == 5);
    assert(jpq_render_focus_podium(&v) == 3);
    jpq_render(&f, &v, &m, 6000, 4000, 0);
    int score_y = 480 - 24 - 44;                       /* bottom_score_y */
    int x0 = (720 - (108 * 6 + 6 * 5)) / 2;            /* scoreboard column 0 */
    assert(pixel_is(&f, x0 + 3 * 114 + 54, score_y, 245, 197, 66));   /* ALEX bordered gold */
    assert(!pixel_is(&f, x0 + 5 * 114 + 54, score_y, 245, 197, 66));  /* not MORGAN */
    CHECKS++;

    /* the winner name is bright green inside a coloured banner */
    v.phase = JV_RESULTS;
    v.leader = 0;
    jpq_render(&f, &v, &m, 6000, 4000, 0);
    assert(pixel_has_green_win(&f));
    CHECKS++;

    /* the final adjudication view shows the correct response (public only once
     * judging begins) without crashing or overlapping */
    v.phase = JV_FINAL_JUDGE;
    v.final.has_response = 1;
    jpq_render(&f, &v, &m, 6000, 4000, 0);
    assert(nonblank(&f) > 10);
    CHECKS++;
    ui_fb_free(&f);
}

static void preview(const char *dir) {
    UiFramebuffer f;
    assert(!ui_fb_init(&f, 720, 480));
    JpqMetadata m;
    jpq_metadata_parse(&m, META, strlen(META));
    const char *names[] = {"lobby", "board", "clue", "daily-double", "final", "final-judge", "results"};
    uint8_t phases[] = {JV_LOBBY, JV_BOARD, JV_CLUE, JV_DD_CLUE, JV_FINAL_CLUE, JV_FINAL_JUDGE, JV_RESULTS};
    for (size_t i = 0; i < sizeof phases; i++) {
        JpqView v;
        preview_build(&v);
        v.phase = phases[i];
        if (phases[i] == JV_DD_CLUE) { v.clue.daily_double = 1; v.buzzed = -1; v.dd_selector = 3; }
        if (phases[i] == JV_FINAL_CLUE || phases[i] == JV_FINAL_JUDGE) v.final.present = 1;
        /* The correct response is public only once judging starts, so the
         * adjudication preview carries the host's reference answer while the
         * clue-only preview does not leak it. */
        if (phases[i] == JV_FINAL_JUDGE) v.final.has_response = 1;
        else if (phases[i] == JV_FINAL_CLUE) v.final.has_response = 0;
        if (phases[i] == JV_RESULTS) {
            v.players[0].score = 4200; v.players[1].score = 2600; v.players[2].score = 1200;
            v.players[3].score = 400; v.players[4].score = -400; v.players[5].score = 1200;
            v.tie = 0; v.leader = 0;
        }
        if (phases[i] == JV_CLUE) { v.timer_kind = JV_TIMER_ANSWER; v.timer_remaining_ms = 6200; }
        jpq_render(&f, &v, &m, 6000, 4000, 0);
        char path[512];
        snprintf(path, sizeof path, "%s/%s.rgba", dir, names[i]);
        FILE *fp = fopen(path, "wb");
        assert(fp);
        fwrite(f.pixels, 1, (size_t)f.width * f.height * 4, fp);
        fclose(fp);
    }
    ui_fb_free(&f);
    printf("previews written to %s\n", dir);
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--preview")) { preview(argv[2]); return 0; }
    test_parse();
    test_render();
    test_remote();
    test_signed_long();
    test_preview_layout();
    test_qr();
    test_lifecycle();
    printf("native: %d checks passed (parse, render, remote, QR, lifecycle)\n", CHECKS);
    return 0;
}
