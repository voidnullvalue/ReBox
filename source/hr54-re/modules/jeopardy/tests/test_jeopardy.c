#define _GNU_SOURCE
#include "../src/jeopardy.h"
#include "../../../reboxd/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static JpqDb db;
static JpqHistory hist;
static JpqDb bigdb;
static JpqHistory bighist;
static JpqDb fixdb;
static JpqHistory fixhist;
static JpqDb gapdb;
static JpqHistory gaphist;
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

static void fresh_game(int players, const JpqConfig *cfg) {
    JpqConfig c;
    if (cfg) c = *cfg; else jpq_config_default(&c);
    jpq_init(&g, &c);
    jpq_attach(&g, &db, &hist);
    jpq_history_reset(&hist);
    for (int i = 0; i < players; i++) {
        char n[16];
        snprintf(n, sizeof n, "P%d", i);
        jpq_add_player(&g, n, 100u + (uint32_t)i);
        jpq_set_ready(&g, (uint8_t)i, 1);
    }
    g.rng = 0x12345678u;
    NOW = 1000;
    SEQ = 0;
    CHECK(apply(JPQ_HOST, JPQ_ACT_START, 0, 0, "") == 0);
}

static int cell_is_dd(const JpqBoard *b, int col, int row) {
    for (uint8_t i = 0; i < b->dd_count; i++)
        if (b->dd_col[i] == col && b->dd_row[i] == row) return 1;
    return 0;
}

static void find_cell(const JpqBoard *b, int want_dd, int *col, int *row) {
    for (int c = 0; c < JPQ_COLS; c++)
        for (int r = 0; r < JPQ_ROWS; r++)
            if (cell_is_dd(b, c, r) == want_dd && b->clue[c][r] >= 0) { *col = c; *row = r; return; }
    *col = -1; *row = -1;
}

/* select, tick past reading, buzz, judge correct, then host-advance out of the
 * answer-reveal hold back to the board (or the next round). */
static void play_correct(int col, int row, int answerer) {
    CHECK(apply(JPQ_HOST, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    if (g.phase == JPQ_PHASE_DD_WAGER) {
        uint8_t sel = g.dd_selector;
        CHECK(apply(sel, JPQ_ACT_WAGER, 0, g.cfg.min_dd_wager, "") == 0);
        CHECK(apply(sel, JPQ_ACT_SUBMIT, 0, 0, "dd") == 0);
        CHECK(apply(JPQ_HOST, JPQ_ACT_JUDGE, sel, JPQ_JUDGE_CORRECT, "") == 0);
        CHECK(g.phase == JPQ_PHASE_REVEAL);
        CHECK(apply(JPQ_HOST, JPQ_ACT_ADVANCE, 0, 0, "") == 0);
        return;
    }
    NOW += g.cfg.read_ms + 1;
    jpq_tick(&g, NOW);
    CHECK(g.timer_kind == JPQ_TIMER_BUZZ);
    CHECK(apply((uint8_t)answerer, JPQ_ACT_BUZZ, 0, 0, "") == 0);
    CHECK(apply(JPQ_HOST, JPQ_ACT_JUDGE, (uint8_t)answerer, JPQ_JUDGE_CORRECT, "") == 0);
    CHECK(g.phase == JPQ_PHASE_REVEAL);
    CHECK(apply(JPQ_HOST, JPQ_ACT_ADVANCE, 0, 0, "") == 0);
}

static void play_boards(int answerer) {
    for (int c = 0; c < JPQ_COLS; c++)
        for (int r = 0; r < JPQ_ROWS; r++)
            play_correct(c, r, answerer);
}

/* ------------------------------------------------------------------ */

static void test_db_load(const char *fixpath) {
    JpqDb d;
    CHECK(jpq_db_load(&d, "/nonexistent/path.jdb") != 0);
    CHECK(jpq_db_load(&d, fixpath) == 0);
    CHECK(d.cat_count == 6);
    CHECK(d.clue_count == 22);
    /* category names are valid UTF-8 strings */
    for (uint32_t i = 0; i < d.cat_count; i++) CHECK(d.cats[i].name && d.cats[i].name[0]);
    /* every clue points to a valid category and has text */
    for (uint32_t i = 0; i < d.clue_count; i++) {
        CHECK(d.clues[i].category < d.cat_count);
        CHECK(d.clues[i].clue && d.clues[i].clue[0]);
        CHECK(d.clues[i].response && d.clues[i].response[0]);
    }
    jpq_db_free(&d);
}

static void test_selection(void) {
    jpq_history_reset(&hist);
    JpqGame t;
    JpqConfig c; jpq_config_default(&c);
    jpq_init(&t, &c);
    jpq_attach(&t, &db, &hist);
    JpqBoard b;
    CHECK(jpq_build_round(&t, NULL, JPQ_ROUND_J, 1234, &b) == 0);
    for (int i = 0; i < JPQ_COLS; i++) {
        CHECK(b.cat[i] >= 0 && b.cat[i] < (int)db.cat_count);
        for (int j = i + 1; j < JPQ_COLS; j++) CHECK(b.cat[i] != b.cat[j]);
        for (int r = 0; r < JPQ_ROWS; r++) {
            CHECK(b.clue[i][r] >= 0);
            CHECK(db.clues[b.clue[i][r]].category == (uint32_t)b.cat[i]);
            CHECK(b.value[i][r] > 0);
            if (r) CHECK(b.value[i][r] > b.value[i][r - 1]);
        }
    }
    CHECK(b.dd_count == c.dd_count_j);
    for (uint8_t d = 0; d < b.dd_count; d++) CHECK(b.dd_row[d] != 0);
    JpqBoard dj;
    CHECK(jpq_build_round(&t, NULL, JPQ_ROUND_DJ, 99, &dj) == 0);
    CHECK(dj.dd_count == c.dd_count_dj);
    for (uint8_t d = 0; d < dj.dd_count; d++) CHECK(dj.dd_row[d] != 0);
    /* filter by round and difficulty */
    JpqFilter f; memset(&f, 0, sizeof f); f.category = -1; f.difficulty_min = 4; f.difficulty_max = 5;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < db.cat_count; i++)
        if (db.cats[i].round == JPQ_ROUND_J && db.cats[i].difficulty >= 4) seen++;
    CHECK(seen > 0);
}

static void test_history(void) {
    jpq_history_reset(&hist);
    JpqGame t;
    JpqConfig c; jpq_config_default(&c);
    jpq_init(&t, &c);
    jpq_attach(&t, &db, &hist);
    JpqBoard a, b;
    CHECK(jpq_build_round(&t, NULL, JPQ_ROUND_J, 11, &a) == 0);
    for (int ci = 0; ci < JPQ_COLS; ci++)
        for (int r = 0; r < JPQ_ROWS; r++)
            CHECK(jpq_history_used(&hist, (uint32_t)a.clue[ci][r]));
    CHECK(jpq_build_round(&t, NULL, JPQ_ROUND_J, 22, &b) == 0);
    /* the two boards never share a clue until the pool is exhausted */
    for (int ci = 0; ci < JPQ_COLS; ci++)
        for (int cj = 0; cj < JPQ_COLS; cj++)
            for (int r = 0; r < JPQ_ROWS; r++)
                for (int s = 0; s < JPQ_ROWS; s++)
                    CHECK(a.clue[ci][r] != b.clue[cj][s]);
    /* a third board exhausts the twelve categories and recycles history */
    JpqBoard c3;
    CHECK(jpq_build_round(&t, NULL, JPQ_ROUND_J, 33, &c3) == 0);
    jpq_reset_history(&t);
    for (int ci = 0; ci < JPQ_COLS; ci++)
        CHECK(!jpq_history_used(&hist, (uint32_t)c3.clue[ci][0]));
}

static void test_filters(void) {
    JpqFilter f;
    memset(&f, 0, sizeof f);
    f.category = -1;
    CHECK(jpq_db_count(&db, &f) == (int)db.cat_count);
    f.year_min = 2030;
    CHECK(jpq_db_count(&db, &f) == 0);
    f.year_min = 2000; f.year_max = 2030;
    CHECK(jpq_db_count(&db, &f) == (int)db.cat_count);
    memset(&f, 0, sizeof f); f.category = 0;
    CHECK(jpq_db_count(&db, &f) == 1);
    memset(&f, 0, sizeof f); f.category = -1; f.difficulty_min = 1; f.difficulty_max = 5;
    CHECK(jpq_db_count(&db, &f) == (int)db.cat_count);
    f.difficulty_min = 6;
    CHECK(jpq_db_count(&db, &f) == 0);

    /* whole-episode board selection stays inside one broadcast */
    jpq_history_reset(&hist);
    JpqGame t;
    JpqConfig c; jpq_config_default(&c);
    jpq_init(&t, &c);
    jpq_attach(&t, &db, &hist);
    JpqFilter ef; memset(&ef, 0, sizeof ef); ef.category = -1; ef.episode = db.cats[0].episode;
    JpqBoard b;
    CHECK(jpq_build_round(&t, &ef, JPQ_ROUND_J, 7, &b) == 0);
    for (int i = 0; i < JPQ_COLS; i++) CHECK(db.cats[b.cat[i]].episode == ef.episode);
}

static void test_lobby(void) {
    JpqConfig c; jpq_config_default(&c);
    jpq_init(&g, &c);
    jpq_attach(&g, &db, &hist);
    jpq_history_reset(&hist);
    NOW = 1000; SEQ = 0;
    for (int i = 0; i < JPQ_MAX_PLAYERS; i++) CHECK(jpq_add_player(&g, "P", (uint32_t)i) == i);
    CHECK(jpq_add_player(&g, "X", 99) == -1);
    CHECK(g.player_count == 6);
    /* start blocked until ready */
    for (int i = 0; i < 6; i++) jpq_set_ready(&g, (uint8_t)i, 1);
    CHECK(apply(JPQ_HOST, JPQ_ACT_START, 0, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_BOARD);
    CHECK(g.player_count == 6);
    /* one player is not enough */
    jpq_init(&g, &c);
    jpq_attach(&g, &db, &hist);
    NOW = 1000; SEQ = 0;
    jpq_add_player(&g, "solo", 1);
    jpq_set_ready(&g, 0, 1);
    CHECK(apply(JPQ_HOST, JPQ_ACT_START, 0, 0, "") != 0);
}

static void test_full_game(void) {
    fresh_game(2, NULL);
    JpqBoard *bj = &g.board_j, *bd = &g.board_dj;
    int64_t expect = 0;
    for (int c = 0; c < JPQ_COLS; c++) for (int r = 0; r < JPQ_ROWS; r++)
        expect += cell_is_dd(bj, c, r) ? g.cfg.min_dd_wager : bj->value[c][r];
    for (int c = 0; c < JPQ_COLS; c++) for (int r = 0; r < JPQ_ROWS; r++)
        expect += cell_is_dd(bd, c, r) ? g.cfg.min_dd_wager : bd->value[c][r];

    play_boards(0);
    /* Double Jeopardy opens with the lowest-scoring player selecting */
    CHECK(g.round == JPQ_ROUND_DJ);
    CHECK(g.selector == 1);
    play_boards(0);
    CHECK(g.phase == JPQ_PHASE_FINAL_WAGER);
    CHECK(g.round == JPQ_ROUND_FINAL);
    /* all 60 board clues consumed */
    int used = 0;
    for (int c = 0; c < JPQ_COLS; c++) for (int r = 0; r < JPQ_ROWS; r++) used += bj->used[c][r] + bd->used[c][r];
    CHECK(used == 60);
    CHECK(g.players[0].score == expect);
    CHECK(g.players[1].score == 0);

    /* final: only positive scores are eligible */
    CHECK(g.eligible[0] == 1 && g.eligible[1] == 0);
    CHECK(apply(1, JPQ_ACT_WAGER, 0, 0, "") != 0); /* ineligible */
    CHECK(apply(0, JPQ_ACT_WAGER, 0, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_FINAL_CLUE);
    CHECK(apply(0, JPQ_ACT_SUBMIT, 0, 0, "answer") == 0);
    CHECK(g.phase == JPQ_PHASE_FINAL_JUDGE);
    CHECK(apply(JPQ_HOST, JPQ_ACT_JUDGE, 0, JPQ_JUDGE_CORRECT, "") == 0);
    CHECK(g.phase == JPQ_PHASE_RESULTS);
    CHECK(g.players[0].score == expect);
}

static void test_dd_constraints(void) {
    fresh_game(2, NULL);
    int col, row;
    find_cell(&g.board_j, 1, &col, &row);
    CHECK(col >= 0);
    g.players[0].score = 5000;
    CHECK(apply(0, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_DD_WAGER);
    CHECK(g.dd_selector == 0);
    CHECK(apply(1, JPQ_ACT_WAGER, 0, 5, "") != 0);      /* not the selector */
    CHECK(apply(0, JPQ_ACT_WAGER, 0, 4, "") != 0);      /* below the floor */
    CHECK(apply(0, JPQ_ACT_WAGER, 0, 5001, "") != 0);   /* above max(score, round max) */
    CHECK(apply(0, JPQ_ACT_WAGER, 0, 5000, "") == 0);   /* exact max */
    CHECK(g.wager[0] == 5000);
    CHECK(g.phase == JPQ_PHASE_DD_CLUE);
    CHECK(apply(0, JPQ_ACT_SUBMIT, 0, 0, "x") == 0);
    CHECK(apply(JPQ_HOST, JPQ_ACT_JUDGE, 0, JPQ_JUDGE_CORRECT, "") == 0);
    CHECK(g.players[0].score == 10000);
}

static void test_dd_timeout(void) {
    fresh_game(2, NULL);
    int col, row;
    find_cell(&g.board_j, 1, &col, &row);
    CHECK(col >= 0);
    CHECK(apply(0, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_DD_WAGER);
    CHECK(g.timer_kind == JPQ_TIMER_DD_WAGER);
    NOW += g.cfg.dd_wager_ms + 1;
    jpq_tick(&g, NOW);
    CHECK(g.phase == JPQ_PHASE_DD_CLUE);
    CHECK(g.wager_set[0] == 1 && g.wager[0] == g.cfg.min_dd_wager);
}

static void test_negative_scores(void) {
    fresh_game(2, NULL);
    int col, row;
    find_cell(&g.board_j, 0, &col, &row);
    int32_t value = g.board_j.value[col][row];
    CHECK(apply(0, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    NOW += g.cfg.read_ms + 1; jpq_tick(&g, NOW);
    CHECK(apply(0, JPQ_ACT_BUZZ, 0, 0, "") == 0);
    CHECK(apply(JPQ_HOST, JPQ_ACT_JUDGE, 0, JPQ_JUDGE_INCORRECT, "") == 0);
    CHECK(g.players[0].score == -value);

    JpqConfig c; jpq_config_default(&c); c.allow_negative = 0;
    fresh_game(2, &c);
    find_cell(&g.board_j, 0, &col, &row);
    CHECK(apply(0, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    NOW += g.cfg.read_ms + 1; jpq_tick(&g, NOW);
    CHECK(apply(0, JPQ_ACT_BUZZ, 0, 0, "") == 0);
    CHECK(apply(JPQ_HOST, JPQ_ACT_JUDGE, 0, JPQ_JUDGE_INCORRECT, "") == 0);
    CHECK(g.players[0].score == 0); /* clamped */
}

static void test_rebuzz(void) {
    fresh_game(3, NULL);
    int col, row;
    find_cell(&g.board_j, 0, &col, &row);
    int32_t value = g.board_j.value[col][row];
    CHECK(apply(0, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    NOW += g.cfg.read_ms + 1; jpq_tick(&g, NOW);
    CHECK(apply(0, JPQ_ACT_BUZZ, 0, 0, "") == 0);
    CHECK(apply(JPQ_HOST, JPQ_ACT_JUDGE, 0, JPQ_JUDGE_INCORRECT, "") == 0);
    CHECK(g.excluded[0] == 1);
    CHECK(g.phase == JPQ_PHASE_CLUE && g.timer_kind == JPQ_TIMER_BUZZ);
    CHECK(apply(0, JPQ_ACT_BUZZ, 0, 0, "") != 0);       /* excluded */
    CHECK(apply(1, JPQ_ACT_BUZZ, 0, 0, "") == 0);
    CHECK(apply(JPQ_HOST, JPQ_ACT_JUDGE, 1, JPQ_JUDGE_CORRECT, "") == 0);
    CHECK(g.phase == JPQ_PHASE_REVEAL);
    CHECK(apply(JPQ_HOST, JPQ_ACT_ADVANCE, 0, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_BOARD);
    CHECK(g.players[0].score == -value);
    CHECK(g.players[1].score == value);
}

static void test_timers(void) {
    fresh_game(2, NULL);
    int col, row;
    find_cell(&g.board_j, 0, &col, &row);
    uint64_t t0 = NOW;
    CHECK(apply(0, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    CHECK(g.timer_kind == JPQ_TIMER_READ);
    jpq_tick(&g, t0 + g.cfg.read_ms - 1);
    CHECK(g.timer_kind == JPQ_TIMER_READ);
    jpq_tick(&g, t0 + g.cfg.read_ms);
    CHECK(g.timer_kind == JPQ_TIMER_BUZZ);
    uint64_t buzz_deadline = g.timer_deadline_ms;
    CHECK(buzz_deadline > 0);
    jpq_tick(&g, buzz_deadline);
    CHECK(g.phase == JPQ_PHASE_REVEAL); /* nobody buzzed: hold the answer */
    CHECK(apply(JPQ_HOST, JPQ_ACT_ADVANCE, 0, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_BOARD);

    /* answer timeout auto-marks incorrect */
    fresh_game(2, NULL);
    find_cell(&g.board_j, 0, &col, &row);
    int32_t value = g.board_j.value[col][row];
    CHECK(apply(0, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    NOW += g.cfg.read_ms + 1; jpq_tick(&g, NOW);
    CHECK(apply(0, JPQ_ACT_BUZZ, 0, 0, "") == 0);
    CHECK(g.timer_kind == JPQ_TIMER_ANSWER);
    jpq_tick(&g, g.timer_deadline_ms);
    CHECK(g.players[0].score == -value);
}

static void test_pause_resume(void) {
    fresh_game(2, NULL);
    int col, row;
    find_cell(&g.board_j, 0, &col, &row);
    uint64_t t0 = NOW;
    CHECK(apply(0, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    NOW = t0 + g.cfg.read_ms / 2;
    CHECK(apply(JPQ_HOST, JPQ_ACT_PAUSE, 0, 0, "") == 0);
    CHECK(g.paused == 1);
    CHECK(g.timer_kind == JPQ_TIMER_READ);
    CHECK(g.timer_remaining_ms > 0 && g.timer_remaining_ms < g.cfg.read_ms);
    CHECK(g.timer_deadline_ms == 0);
    uint64_t remaining = g.timer_remaining_ms;
    jpq_tick(&g, NOW + 1000000);
    CHECK(g.timer_kind == JPQ_TIMER_READ); /* frozen while paused */
    CHECK(apply(JPQ_HOST, JPQ_ACT_RESUME, 0, 0, "") == 0);
    CHECK(g.paused == 0);
    CHECK(g.timer_deadline_ms == NOW + remaining);
    jpq_tick(&g, g.timer_deadline_ms);
    CHECK(g.timer_kind == JPQ_TIMER_BUZZ);
}

static void test_replay_protection(void) {
    JpqConfig c; jpq_config_default(&c);
    jpq_init(&g, &c);
    jpq_attach(&g, &db, &hist);
    jpq_history_reset(&hist);
    NOW = 1000; SEQ = 0;
    jpq_add_player(&g, "A", 1);
    jpq_add_player(&g, "B", 2);
    JpqAction a;
    memset(&a, 0, sizeof a);
    a.seq = 50; a.revision = g.revision; a.epoch = g.epoch;
    a.actor = 0; a.type = JPQ_ACT_READY; a.value = 1;
    CHECK(jpq_apply(&g, &a, NOW) == 0);
    CHECK(jpq_apply(&g, &a, NOW) != 0);           /* duplicate sequence */
    JpqAction b = a; b.seq = 51; b.revision = g.revision + 7;
    CHECK(jpq_apply(&g, &b, NOW) != 0);           /* stale revision */
    b = a; b.seq = 52; b.revision = g.revision; b.epoch = g.epoch + 1;
    CHECK(jpq_apply(&g, &b, NOW) != 0);           /* wrong epoch */
}

static void test_final_private(void) {
    fresh_game(3, NULL);
    /* alternate answers so two players are eligible */
    int idx = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int c = 0; c < JPQ_COLS; c++)
            for (int r = 0; r < JPQ_ROWS; r++) {
                play_correct(c, r, idx % 2);
                idx++;
            }
    CHECK(g.phase == JPQ_PHASE_FINAL_WAGER);
    CHECK(g.eligible[0] && g.eligible[1] && !g.eligible[2]);
    CHECK(apply(0, JPQ_ACT_WAGER, 0, 100, "") == 0);
    CHECK(apply(2, JPQ_ACT_WAGER, 0, 50, "") != 0); /* ineligible */
    CHECK(apply(1, JPQ_ACT_WAGER, 0, 200, "") == 0);
    CHECK(g.phase == JPQ_PHASE_FINAL_CLUE);
    CHECK(apply(0, JPQ_ACT_SUBMIT, 0, 0, "alpha") == 0);
    CHECK(apply(1, JPQ_ACT_SUBMIT, 0, 0, "beta") == 0);
    CHECK(g.phase == JPQ_PHASE_FINAL_JUDGE);
    CHECK(g.judge_cursor == 0);

    /* private data: viewer 0 must not see player 1's wager or answer */
    char *js = NULL; size_t jn = 0;
    CHECK(jpq_snapshot_json(&g, 0, &js, &jn) == 0);
    struct jval *root = json_parse(js, jn);
    CHECK(root && root->t == J_OBJ);
    struct jval *players = jget(root, "players");
    CHECK(players && players->t == J_ARR && players->n == 3);
    struct jval *p0 = jnth(players, 0), *p1 = jnth(players, 1);
    CHECK(jget(p0, "wager")->t == J_NUM && jnum(jget(p0, "wager"), -1) == 100);
    CHECK(jget(p0, "answer")->t == J_STR && !strcmp(jstr(jget(p0, "answer")), "alpha"));
    CHECK(jget(p1, "wager")->t == J_NULL);
    CHECK(jget(p1, "answer")->t == J_NULL);
    jfree(root); free(js);

    /* host sees the current answer only, sequentially */
    js = NULL;
    CHECK(jpq_snapshot_json(&g, JPQ_HOST, &js, &jn) == 0);
    root = json_parse(js, jn);
    players = jget(root, "players");
    CHECK(jget(jnth(players, 0), "answer")->t == J_STR); /* being judged now */
    CHECK(jget(jnth(players, 1), "answer")->t == J_NULL); /* not yet */
    jfree(root); free(js);

    /* sequential judging */
    CHECK(apply(JPQ_HOST, JPQ_ACT_JUDGE, 1, JPQ_JUDGE_CORRECT, "") != 0); /* out of order */
    CHECK(apply(JPQ_HOST, JPQ_ACT_JUDGE, 0, JPQ_JUDGE_CORRECT, "") == 0);
    CHECK(g.judge_cursor == 1);
    CHECK(apply(JPQ_HOST, JPQ_ACT_JUDGE, 1, JPQ_JUDGE_INCORRECT, "") == 0);
    CHECK(g.phase == JPQ_PHASE_RESULTS);
}

static void test_ties_tiebreak(void) {
    fresh_game(2, NULL);
    g.phase = JPQ_PHASE_RESULTS;
    g.players[0].score = 1000;
    g.players[1].score = 1000;
    CHECK(jpq_tie_count(&g) == 2);
    CHECK(apply(JPQ_HOST, JPQ_ACT_TIEBREAK, 0, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_TIEBREAK_CLUE);
    CHECK(g.tiebreak_active == 1);
    CHECK(g.eligible[0] && g.eligible[1]);
    NOW += g.cfg.read_ms + 1; jpq_tick(&g, NOW);
    CHECK(g.timer_kind == JPQ_TIMER_BUZZ);
    CHECK(apply(0, JPQ_ACT_BUZZ, 0, 0, "") == 0);
    CHECK(apply(JPQ_HOST, JPQ_ACT_JUDGE, 0, JPQ_JUDGE_CORRECT, "") == 0);
    CHECK(g.phase == JPQ_PHASE_RESULTS);
    CHECK(g.tiebreak_winner == 0);
    /* sudden death never moves money, only marks the winner */
    CHECK(g.players[0].score == 1000 && g.players[1].score == 1000);
    CHECK(jpq_leader(&g) == 0);
}

static void test_host_actions(void) {
    fresh_game(2, NULL);
    int before = g.players[0].score;
    CHECK(apply(JPQ_HOST, JPQ_ACT_SCORE, 0, 500, "") == 0);
    CHECK(g.players[0].score == before + 500);

    /* pass and reveal */
    int col, row;
    find_cell(&g.board_j, 0, &col, &row);
    CHECK(apply(0, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    NOW += g.cfg.read_ms + 1; jpq_tick(&g, NOW);
    CHECK(apply(0, JPQ_ACT_BUZZ, 0, 0, "") == 0);
    CHECK(apply(JPQ_HOST, JPQ_ACT_PASS, 0, 0, "") == 0);
    CHECK(g.excluded[0] == 1 && g.phase == JPQ_PHASE_CLUE);
    CHECK(apply(1, JPQ_ACT_BUZZ, 0, 0, "") == 0);
    CHECK(apply(JPQ_HOST, JPQ_ACT_REVEAL, 0, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_REVEAL); /* response held for the host */
    CHECK(apply(JPQ_HOST, JPQ_ACT_ADVANCE, 0, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_BOARD);

    /* restart needs confirmation */
    CHECK(apply(JPQ_HOST, JPQ_ACT_RESTART, 0, 0, "") == 0);
    CHECK(g.restart_confirm == 1 && g.phase != JPQ_PHASE_LOBBY);
    CHECK(apply(JPQ_HOST, JPQ_ACT_RESTART, 0, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_LOBBY);
    CHECK(g.players[0].score == 0);

    /* quit needs confirmation */
    CHECK(apply(JPQ_HOST, JPQ_ACT_QUIT, 0, 0, "") == 0);
    CHECK(g.quit == 0);
    CHECK(apply(JPQ_HOST, JPQ_ACT_QUIT, 0, 0, "") == 0);
    CHECK(g.quit == 1);
}

static void test_persistence(const char *tmp) {
    char dir[256];
    snprintf(dir, sizeof dir, "%s/persist", tmp);
    mkdir(dir, 0700);
    fresh_game(2, NULL);
    int col, row;
    find_cell(&g.board_j, 0, &col, &row);
    uint64_t t0 = NOW;
    CHECK(apply(0, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    NOW = t0 + g.cfg.read_ms / 3;
    CHECK(apply(JPQ_HOST, JPQ_ACT_PAUSE, 0, 0, "") == 0);
    uint64_t remaining = g.timer_remaining_ms;
    CHECK(jpq_save(&g, dir) == 0);

    JpqGame l;
    memset(&l, 0, sizeof l);
    jpq_attach(&l, &db, &hist);
    CHECK(jpq_load(&l, dir) == 0);
    CHECK(l.paused == 1);                     /* in-progress game resumes paused */
    CHECK(l.timer_deadline_ms == 0);          /* no monotonic deadline persisted */
    CHECK(l.timer_remaining_ms == remaining);
    CHECK(l.timer_kind == g.timer_kind);
    CHECK(l.phase == g.phase);
    CHECK(l.revision == g.revision);
    CHECK(l.epoch == g.epoch);
    CHECK(l.player_count == g.player_count);
    CHECK(l.players[0].score == g.players[0].score);
    CHECK(!memcmp(l.players[0].name, g.players[0].name, JPQ_NAME_MAX));
    CHECK(!memcmp(&l.board_j, &g.board_j, sizeof g.board_j));
    CHECK(!memcmp(&l.board_dj, &g.board_dj, sizeof g.board_dj));

    /* a paused reload stays frozen */
    uint64_t rev = l.revision;
    jpq_tick(&l, NOW + 1000000);
    CHECK(l.revision == rev);

    /* corrupt save is quarantined and reported */
    char path[300], backup[300];
    snprintf(path, sizeof path, "%s/jeopardy-v1.bin", dir);
    FILE *f = fopen(path, "r+b");
    CHECK(f != NULL);
    if (f) { fputc(0, f); fclose(f); }
    CHECK(jpq_load(&l, dir) < 0);
    int found = 0;
    for (int i = 0; i < 100; i++) {
        snprintf(backup, sizeof backup, "%s/jeopardy-v1.invalid-%02d.bin", dir, i);
        if (!access(backup, F_OK)) { found = 1; break; }
    }
    CHECK(found);

    /* missing save starts fresh */
    char empty[256];
    snprintf(empty, sizeof empty, "%s/empty", tmp);
    mkdir(empty, 0700);
    JpqGame e;
    memset(&e, 0, sizeof e);
    jpq_attach(&e, &db, &hist);
    CHECK(jpq_load(&e, empty) == -1);
}

static void test_snapshot(void) {
    fresh_game(2, NULL);
    char *js = NULL; size_t jn = 0;
    CHECK(jpq_snapshot_json(&g, 0, &js, &jn) == 0);
    struct jval *root = json_parse(js, jn);
    CHECK(root && root->t == J_OBJ);
    CHECK((uint32_t)jnum(jget(root, "revision"), 0) == g.revision);
    CHECK((uint32_t)jnum(jget(root, "phase"), 99) == g.phase);
    struct jval *board = jget(root, "board");
    CHECK(board && board->t == J_OBJ);
    struct jval *cats = jget(board, "categories");
    CHECK(cats && cats->t == J_ARR && cats->n == JPQ_COLS);
    struct jval *c0 = jnth(cats, 0);
    struct jval *clues = jget(c0, "clues");
    CHECK(clues && clues->n == JPQ_ROWS);
    jfree(root); free(js);
}

/* ------------------------------------------------------------------ */
/* Stage-1 review repairs                                           */
/* ------------------------------------------------------------------ */

static void test_authentic_values(void) {
    JpqGame t;
    JpqConfig c; jpq_config_default(&c);
    jpq_init(&t, &c);
    jpq_attach(&t, &db, &hist);
    jpq_history_reset(&hist);
    JpqBoard bj, bd;
    CHECK(jpq_build_round(&t, NULL, JPQ_ROUND_J, 5, &bj) == 0);
    for (int c2 = 0; c2 < JPQ_COLS; c2++)
        for (int r = 0; r < JPQ_ROWS; r++) CHECK(bj.value[c2][r] == 200 * (r + 1));
    CHECK(jpq_build_round(&t, NULL, JPQ_ROUND_DJ, 6, &bd) == 0);
    for (int c2 = 0; c2 < JPQ_COLS; c2++)
        for (int r = 0; r < JPQ_ROWS; r++) CHECK(bd.value[c2][r] == 400 * (r + 1));
    /* the database still carries the original broadcast values */
    int found = 0;
    for (uint32_t i = 0; i < db.clue_count; i++) if (db.clues[i].value > 0) found = 1;
    CHECK(found);
}

static void test_early_buzz(void) {
    fresh_game(2, NULL);
    int col, row; find_cell(&g.board_j, 0, &col, &row);
    CHECK(apply(0, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    CHECK(g.timer_kind == JPQ_TIMER_READ);
    uint32_t rev = g.revision;
    CHECK(apply(0, JPQ_ACT_BUZZ, 0, 0, "") != 0);   /* read not finished */
    CHECK(g.revision == rev);                        /* no mutation */
    NOW += g.cfg.read_ms + 1; jpq_tick(&g, NOW);
    CHECK(g.timer_kind == JPQ_TIMER_BUZZ);
    CHECK(apply(0, JPQ_ACT_BUZZ, 0, 0, "") == 0);
}

static void test_seq_wide(void) {
    fresh_game(2, NULL);
    for (int i = 0; i < 300; i++) CHECK(apply(JPQ_HOST, JPQ_ACT_SCORE, 0, 0, "") == 0);
    CHECK(SEQ > 300);
    JpqAction a;
    memset(&a, 0, sizeof a);
    a.seq = 200; a.revision = g.revision; a.epoch = g.epoch;
    a.actor = JPQ_HOST; a.type = JPQ_ACT_SCORE; a.target = 0;
    CHECK(jpq_apply(&g, &a, NOW) != 0);      /* replayed id long past 255 */
    a.seq = SEQ + 1;
    CHECK(jpq_apply(&g, &a, NOW) == 0);      /* a genuinely newer id */
}

static void test_invalid_no_mutation(void) {
    /* duplicate Final wager and answer are rejected without mutation */
    fresh_game(3, NULL);
    int idx = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int c = 0; c < JPQ_COLS; c++)
            for (int r = 0; r < JPQ_ROWS; r++) { play_correct(c, r, idx % 2); idx++; }
    CHECK(g.phase == JPQ_PHASE_FINAL_WAGER);
    CHECK(g.eligible[0] && g.eligible[1] && !g.eligible[2]);
    CHECK(apply(0, JPQ_ACT_WAGER, 0, 100, "") == 0);
    uint32_t rev = g.revision; int32_t w = g.wager[0];
    CHECK(apply(0, JPQ_ACT_WAGER, 0, 50, "") != 0);
    CHECK(g.wager[0] == w && g.revision == rev);
    CHECK(apply(1, JPQ_ACT_WAGER, 0, 50, "") == 0);
    CHECK(g.phase == JPQ_PHASE_FINAL_CLUE);
    CHECK(apply(0, JPQ_ACT_SUBMIT, 0, 0, "a") == 0);
    rev = g.revision;
    CHECK(apply(0, JPQ_ACT_SUBMIT, 0, 0, "b") != 0);
    CHECK(!strcmp(g.answer[0], "a") && g.revision == rev);
    CHECK(apply(1, JPQ_ACT_SUBMIT, 0, 0, "c") == 0);
    CHECK(g.phase == JPQ_PHASE_FINAL_JUDGE);

    /* a failing START leaves the lobby and boards untouched */
    JpqConfig c2; jpq_config_default(&c2);
    jpq_init(&g, &c2);
    jpq_attach(&g, &fixdb, &fixhist);
    jpq_history_reset(&fixhist);
    NOW = 1000; SEQ = 0;
    jpq_add_player(&g, "A", 1); jpq_add_player(&g, "B", 2);
    jpq_set_ready(&g, 0, 1); jpq_set_ready(&g, 1, 1);
    uint32_t rev0 = g.revision, ep0 = g.epoch;
    CHECK(apply(JPQ_HOST, JPQ_ACT_START, 0, 0, "") != 0);
    CHECK(g.phase == JPQ_PHASE_LOBBY);
    CHECK(g.revision == rev0 && g.epoch == ep0);
    CHECK(g.board_j.clue[0][0] == 0 && g.board_j.cat[0] == 0);

    /* a failing restart build leaves scores and phase untouched */
    fresh_game(2, NULL);
    CHECK(apply(JPQ_HOST, JPQ_ACT_SCORE, 0, 500, "") == 0);
    int32_t s0 = g.players[0].score;
    uint8_t ph = g.phase;
    JpqDb *saved = g.db;
    g.db = &fixdb;
    CHECK(apply(JPQ_HOST, JPQ_ACT_RESTART, 0, 0, "") == 0);   /* arm */
    CHECK(apply(JPQ_HOST, JPQ_ACT_RESTART, 0, 0, "") != 0);   /* build fails */
    g.db = saved;
    CHECK(g.players[0].score == s0);
    CHECK(g.phase == ph);
    CHECK(g.restart_confirm == 1);
}

static int snapshot_dd_count(uint8_t viewer) {
    char *js = NULL; size_t jn = 0;
    if (jpq_snapshot_json(&g, viewer, &js, &jn)) return -1;
    struct jval *root = json_parse(js, jn);
    int n = 0;
    struct jval *cats = jget(jget(root, "board"), "categories");
    if (cats && cats->t == J_ARR)
        for (size_t ci = 0; ci < cats->n; ci++) {
            struct jval *clues = jget(jnth(cats, ci), "clues");
            if (!clues) continue;
            for (size_t r = 0; r < clues->n; r++)
                if (jnum(jget(jnth(clues, r), "daily_double"), 0) > 0) n++;
        }
    jfree(root); free(js);
    return n;
}

static void test_snapshot_redaction(void) {
    CHECK(JPQ_PUBLIC != JPQ_HOST);
    fresh_game(2, NULL);
    /* unused daily-double locations stay host-only */
    CHECK(snapshot_dd_count(JPQ_PUBLIC) == 0);
    CHECK(snapshot_dd_count(0) == 0);
    CHECK(snapshot_dd_count(JPQ_HOST) == g.board_j.dd_count);

    /* the daily-double clue text is hidden while the selector wagers */
    int col, row; find_cell(&g.board_j, 1, &col, &row);
    CHECK(apply(JPQ_HOST, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_DD_WAGER);
    char *js = NULL; size_t jn = 0;
    CHECK(jpq_snapshot_json(&g, JPQ_PUBLIC, &js, &jn) == 0);
    struct jval *root = json_parse(js, jn);
    CHECK(jget(jget(root, "clue"), "text")->t == J_NULL);
    jfree(root); free(js);
    CHECK(apply(0, JPQ_ACT_WAGER, 0, g.cfg.min_dd_wager, "") == 0);
    js = NULL;
    CHECK(jpq_snapshot_json(&g, JPQ_PUBLIC, &js, &jn) == 0);
    root = json_parse(js, jn);
    CHECK(jget(jget(root, "clue"), "text")->t == J_STR);
    jfree(root); free(js);

    /* Final: category at FJ, text only after wagering, sequential judging */
    fresh_game(2, NULL);
    int32_t fc = -1;
    for (uint32_t i = 0; i < db.cat_count; i++)
        if (db.cats[i].round == JPQ_ROUND_FINAL) { fc = (int32_t)db.cats[i].first_clue; break; }
    CHECK(fc >= 0);
    g.round = JPQ_ROUND_FINAL;
    g.phase = JPQ_PHASE_FINAL_WAGER;
    g.final_clue = fc;
    g.eligible[0] = g.eligible[1] = 1;
    g.wager[0] = 100; g.wager[1] = 200;
    snprintf(g.answer[0], sizeof g.answer[0], "a0");
    snprintf(g.answer[1], sizeof g.answer[1], "a1");

    js = NULL;
    CHECK(jpq_snapshot_json(&g, JPQ_PUBLIC, &js, &jn) == 0);
    root = json_parse(js, jn);
    CHECK(jget(jget(root, "final"), "category")->t == J_STR);
    CHECK(jget(jget(root, "final"), "text")->t == J_NULL);
    CHECK(jget(jnth(jget(root, "players"), 0), "wager")->t == J_NULL);
    jfree(root); free(js);

    g.phase = JPQ_PHASE_FINAL_CLUE;
    js = NULL;
    CHECK(jpq_snapshot_json(&g, JPQ_PUBLIC, &js, &jn) == 0);
    root = json_parse(js, jn);
    CHECK(jget(jget(root, "final"), "text")->t == J_STR);
    jfree(root); free(js);

    g.phase = JPQ_PHASE_FINAL_JUDGE;
    g.judge_cursor = 0; g.judged[0] = 0; g.judged[1] = 0;
    js = NULL;
    CHECK(jpq_snapshot_json(&g, JPQ_PUBLIC, &js, &jn) == 0);
    root = json_parse(js, jn);
    struct jval *players = jget(root, "players");
    CHECK(jget(jnth(players, 0), "wager")->t == J_NUM);   /* current */
    CHECK(jget(jnth(players, 0), "answer")->t == J_STR);
    CHECK(jget(jnth(players, 1), "wager")->t == J_NULL);  /* not yet */
    CHECK(jget(jnth(players, 1), "answer")->t == J_NULL);
    jfree(root); free(js);

    g.judged[0] = 1; g.judge_cursor = 1;
    js = NULL;
    CHECK(jpq_snapshot_json(&g, JPQ_PUBLIC, &js, &jn) == 0);
    root = json_parse(js, jn);
    players = jget(root, "players");
    CHECK(jget(jnth(players, 0), "wager")->t == J_NUM);   /* already judged */
    CHECK(jget(jnth(players, 1), "wager")->t == J_NUM);
    jfree(root); free(js);
}

static void test_episode_archive(void) {
    CHECK(bigdb.cat_count > 127);
    CHECK(bigdb.clue_count >= 1000);
    JpqConfig c; jpq_config_default(&c); c.random_categories = 0;
    jpq_init(&g, &c);
    jpq_attach(&g, &bigdb, &bighist);
    jpq_history_reset(&bighist);
    g.rng = 0xABCDEF01u; NOW = 1000; SEQ = 0;
    jpq_add_player(&g, "A", 1); jpq_add_player(&g, "B", 2);
    jpq_set_ready(&g, 0, 1); jpq_set_ready(&g, 1, 1);
    CHECK(apply(JPQ_HOST, JPQ_ACT_START, 0, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_BOARD && g.round == JPQ_ROUND_J);
    uint32_t ep = bigdb.cats[g.board_j.cat[0]].episode;
    for (int c2 = 0; c2 < JPQ_COLS; c2++) {
        CHECK(bigdb.cats[g.board_j.cat[c2]].episode == ep);
        CHECK(bigdb.cats[g.board_dj.cat[c2]].episode == ep);
        CHECK(g.board_j.cat[c2] > 127);           /* high category indices */
        CHECK(g.board_dj.cat[c2] > 127);
    }
    /* historical daily-double locations come from the archived flags */
    for (uint8_t d = 0; d < g.board_j.dd_count; d++) {
        int cc = g.board_j.dd_col[d], rr = g.board_j.dd_row[d];
        CHECK(bigdb.clues[g.board_j.clue[cc][rr]].flags & JPQ_QFLAG_DD);
    }
    for (uint8_t d = 0; d < g.board_dj.dd_count; d++) {
        int cc = g.board_dj.dd_col[d], rr = g.board_dj.dd_row[d];
        CHECK(bigdb.clues[g.board_dj.clue[cc][rr]].flags & JPQ_QFLAG_DD);
    }
    uint32_t fid = (uint32_t)g.final_clue;
    CHECK(fid < bigdb.clue_count);
    CHECK(bigdb.cats[bigdb.clues[fid].category].episode == ep);
}

static void test_large_indices(const char *tmp) {
    char dir[256];
    snprintf(dir, sizeof dir, "%s/large", tmp);
    mkdir(dir, 0700);
    JpqConfig c; jpq_config_default(&c);
    jpq_init(&g, &c);
    jpq_attach(&g, &bigdb, &bighist);
    jpq_history_reset(&bighist);
    g.rng = 0x55AA55AAu; NOW = 1000; SEQ = 0;
    jpq_add_player(&g, "A", 1); jpq_add_player(&g, "B", 2);
    jpq_set_ready(&g, 0, 1); jpq_set_ready(&g, 1, 1);
    CHECK(apply(JPQ_HOST, JPQ_ACT_START, 0, 0, "") == 0);
    int32_t hi_cat = -1, hi_clue = -1;
    for (int c2 = 0; c2 < JPQ_COLS; c2++) {
        if (g.board_j.cat[c2] > hi_cat) hi_cat = g.board_j.cat[c2];
        for (int r = 0; r < JPQ_ROWS; r++)
            if (g.board_j.clue[c2][r] > hi_clue) hi_clue = g.board_j.clue[c2][r];
    }
    CHECK(hi_cat > 127);
    CHECK(hi_clue > 127);

    /* high indices survive a versioned persistence round-trip */
    CHECK(jpq_save(&g, dir) == 0);
    JpqGame l; memset(&l, 0, sizeof l);
    jpq_attach(&l, &bigdb, &bighist);
    CHECK(jpq_load(&l, dir) == 0);
    CHECK(!memcmp(&l.board_j, &g.board_j, sizeof g.board_j));
    CHECK(!memcmp(&l.board_dj, &g.board_dj, sizeof g.board_dj));
    CHECK(l.final_clue == g.final_clue);
    CHECK(l.final_clue < (int32_t)bigdb.clue_count);

    /* invalid indices are empty/rejected, never read out of bounds */
    CHECK(strcmp(jpq_db_clue_text(&bigdb, bigdb.clue_count), "") == 0);
    CHECK(strcmp(jpq_db_clue_response(&bigdb, 0xFFFFFFFFu), "") == 0);
    JpqFilter f; memset(&f, 0, sizeof f); f.category = (int)bigdb.cat_count + 5;
    CHECK(jpq_db_count(&bigdb, &f) == 0);
    JpqGame t; jpq_init(&t, &c); jpq_attach(&t, &bigdb, &bighist);
    JpqBoard b;
    CHECK(jpq_build_round(&t, &f, JPQ_ROUND_J, 3, &b) != 0);
}

static void test_final_history_preserved(void) {
    jpq_history_reset(&hist);
    JpqGame t; JpqConfig c; jpq_config_default(&c);
    jpq_init(&t, &c); jpq_attach(&t, &db, &hist);
    JpqBoard b;
    CHECK(jpq_build_round(&t, NULL, JPQ_ROUND_J, 1, &b) == 0);
    uint32_t jc = (uint32_t)b.clue[0][0];
    CHECK(jpq_history_used(&hist, jc));
    int built = 0;
    for (int i = 0; i < 5; i++) { int32_t f; if (jpq_build_final(&t, NULL, (uint32_t)(100 + i), &f) == 0) built++; }
    CHECK(built == 5);
    /* the Final pool is now exhausted: J history must survive the reuse */
    int32_t f;
    CHECK(jpq_build_final(&t, NULL, 999, &f) == 0);
    CHECK(jpq_history_used(&hist, jc));
}

static void test_filter_at_start(void) {
    JpqFilter f; memset(&f, 0, sizeof f); f.category = -1;
    uint32_t ep = db.cats[0].episode;
    f.episode = ep;
    fresh_game(2, NULL);
    CHECK(jpq_set_filter(&g, &f) == 0);
    /* re-run START with the stored filter and check it was honored */
    /* fresh_game already started; rebuild by restarting through the engine */
    jpq_init(&g, NULL);
    jpq_attach(&g, &db, &hist);
    jpq_history_reset(&hist);
    jpq_add_player(&g, "A", 1); jpq_add_player(&g, "B", 2);
    jpq_set_ready(&g, 0, 1); jpq_set_ready(&g, 1, 1);
    g.rng = 0x12345678u; NOW = 1000; SEQ = 0;
    CHECK(jpq_set_filter(&g, &f) == 0);
    CHECK(apply(JPQ_HOST, JPQ_ACT_START, 0, 0, "") == 0);
    for (int c2 = 0; c2 < JPQ_COLS; c2++) CHECK(db.cats[g.board_j.cat[c2]].episode == ep);
    CHECK(jpq_set_filter(&g, NULL) == 0);
    CHECK(g.filter_set == 0);
}

static void test_selector_control(void) {
    /* only the current controller (or the host on their behalf) may select */
    fresh_game(3, NULL);
    CHECK(g.selector == 0);
    int col, row; find_cell(&g.board_j, 0, &col, &row);
    uint32_t rev = g.revision;
    CHECK(apply(1, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") != 0);
    CHECK(apply(2, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") != 0);
    CHECK(g.selector == 0 && g.revision == rev && g.phase == JPQ_PHASE_BOARD);
    CHECK(apply(0, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    CHECK(g.act_col == col && g.act_row == row);

    /* the host may select on the controller's behalf without changing it */
    fresh_game(3, NULL);
    g.selector = 2;
    find_cell(&g.board_j, 0, &col, &row);
    CHECK(apply(JPQ_HOST, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    CHECK(g.selector == 2);
}

static void test_inactive_actors(void) {
    /* lobby: an inactive contestant cannot ready */
    JpqConfig c; jpq_config_default(&c);
    jpq_init(&g, &c);
    jpq_attach(&g, &db, &hist);
    jpq_history_reset(&hist);
    NOW = 1000; SEQ = 0;
    jpq_add_player(&g, "A", 1); jpq_add_player(&g, "B", 2);
    g.players[0].active = 0;
    CHECK(apply(0, JPQ_ACT_READY, 0, 1, "") != 0);
    CHECK(g.players[0].ready == 0);

    /* board: inactive cannot select or buzz */
    fresh_game(3, NULL);
    int col, row; find_cell(&g.board_j, 0, &col, &row);
    g.players[1].active = 0;
    g.selector = 1;
    CHECK(apply(1, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") != 0);
    g.selector = 0;
    CHECK(apply(0, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    NOW += g.cfg.read_ms + 1; jpq_tick(&g, NOW);
    CHECK(apply(1, JPQ_ACT_BUZZ, 0, 0, "") != 0);

    /* DD: inactive cannot wager or submit */
    fresh_game(2, NULL);
    find_cell(&g.board_j, 1, &col, &row);
    CHECK(apply(0, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_DD_WAGER);
    g.players[0].active = 0;
    CHECK(apply(0, JPQ_ACT_WAGER, 0, g.cfg.min_dd_wager, "") != 0);
    g.players[0].active = 1;
    CHECK(apply(0, JPQ_ACT_WAGER, 0, g.cfg.min_dd_wager, "") == 0);
    CHECK(g.phase == JPQ_PHASE_DD_CLUE);
    g.players[0].active = 0;
    CHECK(apply(0, JPQ_ACT_SUBMIT, 0, 0, "x") != 0);
}

static void test_round_scoped_recycling(void) {
    /* recycling an exhausted round must not wipe the other rounds' history */
    jpq_history_reset(&hist);
    JpqGame t; JpqConfig c; jpq_config_default(&c);
    jpq_init(&t, &c); jpq_attach(&t, &db, &hist);
    JpqBoard j1, j2, dj1, j3;
    CHECK(jpq_build_round(&t, NULL, JPQ_ROUND_J, 1, &j1) == 0);
    CHECK(jpq_build_round(&t, NULL, JPQ_ROUND_J, 2, &j2) == 0);
    CHECK(jpq_build_round(&t, NULL, JPQ_ROUND_DJ, 3, &dj1) == 0);
    uint32_t dj_clue = (uint32_t)dj1.clue[0][0];
    CHECK(jpq_history_used(&hist, dj_clue));
    CHECK(jpq_build_round(&t, NULL, JPQ_ROUND_J, 4, &j3) == 0); /* recycle J only */
    CHECK(jpq_history_used(&hist, dj_clue));
}

static void test_episode_mode_freshness(void) {
    JpqConfig c; jpq_config_default(&c); c.random_categories = 0;
    jpq_init(&g, &c); jpq_attach(&g, &bigdb, &bighist); jpq_history_reset(&bighist);
    g.rng = 0x12345678u; NOW = 1000; SEQ = 0;
    jpq_add_player(&g, "A", 1); jpq_add_player(&g, "B", 2);
    jpq_set_ready(&g, 0, 1); jpq_set_ready(&g, 1, 1);
    CHECK(apply(JPQ_HOST, JPQ_ACT_START, 0, 0, "") == 0);
    /* the random episode is not written back into the configured filter */
    CHECK(g.filter_set == 0);
    CHECK(g.filter.episode == 0);
    uint32_t ep = bigdb.cats[g.board_j.cat[0]].episode;

    /* exhaust that episode, then a new game must choose the other fresh one */
    for (uint32_t i = 0; i < bigdb.cat_count; i++)
        if (bigdb.cats[i].episode == ep)
            for (uint16_t k = 0; k < bigdb.cats[i].clue_count; k++)
                jpq_history_mark(&bighist, bigdb.cats[i].first_clue + k);
    jpq_init(&g, &c); jpq_attach(&g, &bigdb, &bighist);
    g.rng = 0x87654321u; NOW = 1000; SEQ = 0;
    jpq_add_player(&g, "A", 1); jpq_add_player(&g, "B", 2);
    jpq_set_ready(&g, 0, 1); jpq_set_ready(&g, 1, 1);
    CHECK(apply(JPQ_HOST, JPQ_ACT_START, 0, 0, "") == 0);
    uint32_t ep2 = bigdb.cats[g.board_j.cat[0]].episode;
    CHECK(ep2 != ep);
}

static void test_explicit_episode_fail(void) {
    jpq_history_reset(&hist);
    JpqGame t; JpqConfig c; jpq_config_default(&c);
    jpq_init(&t, &c); jpq_attach(&t, &db, &hist);
    int cat = -1;
    for (uint32_t i = 0; i < db.cat_count; i++)
        if (db.cats[i].round == JPQ_ROUND_J && db.cats[i].clue_count >= JPQ_ROWS) { cat = (int)i; break; }
    CHECK(cat >= 0);
    JpqBoard b;
    /* a single category (incomplete episode board) must fail, not fall back */
    JpqFilter f; memset(&f, 0, sizeof f); f.category = cat; f.episode = db.cats[cat].episode;
    CHECK(jpq_build_round(&t, &f, JPQ_ROUND_J, 1, &b) != 0);
    /* a nonexistent episode fails as well */
    JpqFilter g2; memset(&g2, 0, sizeof g2); g2.category = -1; g2.episode = 0xFFFFFFFFu;
    CHECK(jpq_build_round(&t, &g2, JPQ_ROUND_J, 1, &b) != 0);
    /* a complete explicit episode still builds from one broadcast */
    JpqFilter g3; memset(&g3, 0, sizeof g3); g3.category = -1; g3.episode = db.cats[cat].episode;
    CHECK(jpq_build_round(&t, &g3, JPQ_ROUND_J, 1, &b) == 0);
    for (int i = 0; i < JPQ_COLS; i++) CHECK(db.cats[b.cat[i]].episode == g3.episode);
}

static void test_episode_mode_requires_complete(void) {
    /* episode (non-random) mode must fail rather than degrade into unrelated
     * random categories when no complete episode exists in the database */
    JpqGame t; JpqConfig c; jpq_config_default(&c); c.random_categories = 0;
    jpq_init(&t, &c); jpq_attach(&t, &gapdb, &gaphist); jpq_history_reset(&gaphist);
    CHECK(jpq_build_boards(&t, 1234) != 0);
    /* the same holds for a configured non-episode filter */
    JpqFilter f; memset(&f, 0, sizeof f); f.category = -1;
    CHECK(jpq_set_filter(&t, &f) == 0);
    CHECK(jpq_build_boards(&t, 1234) != 0);
    /* the gap database can build random boards: only the episode logic fails */
    JpqConfig c2; jpq_config_default(&c2);
    jpq_init(&t, &c2); jpq_attach(&t, &gapdb, &gaphist); jpq_history_reset(&gaphist);
    CHECK(jpq_build_boards(&t, 4321) == 0);
}

static void test_persist_validation(const char *tmp) {
    char dir[256];
    JpqGame l;

    /* a save written against a different database is quarantined */
    snprintf(dir, sizeof dir, "%s/pv_large", tmp); mkdir(dir, 0700);
    JpqConfig c; jpq_config_default(&c);
    jpq_init(&g, &c); jpq_attach(&g, &bigdb, &bighist); jpq_history_reset(&bighist);
    g.rng = 0x1111u; NOW = 1000; SEQ = 0;
    jpq_add_player(&g, "A", 1); jpq_add_player(&g, "B", 2);
    jpq_set_ready(&g, 0, 1); jpq_set_ready(&g, 1, 1);
    CHECK(apply(JPQ_HOST, JPQ_ACT_START, 0, 0, "") == 0);
    CHECK(jpq_save(&g, dir) == 0);
    memset(&l, 0, sizeof l); jpq_attach(&l, &db, &hist);
    CHECK(jpq_load(&l, dir) < 0);

    /* incoherent board/state fields are quarantined, never loaded */
    struct { const char *tag; int what; } cases[] = {
        {"cat", 0}, {"clue", 1}, {"dd", 2}, {"selector", 3}, {"act", 4}
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char d[256];
        snprintf(d, sizeof d, "%s/pv_%s", tmp, cases[i].tag); mkdir(d, 0700);
        fresh_game(2, NULL);
        switch (cases[i].what) {
        case 0: g.board_j.cat[0] = (int32_t)db.cat_count + 3; break;
        case 1: g.board_j.clue[0][0] = (int32_t)db.clue_count + 1; break;
        case 2: g.board_j.dd_col[0] = (uint8_t)(JPQ_COLS + 1); break;
        case 3: g.selector = 9; break;
        case 4: g.act_col = JPQ_COLS + 2; break;
        }
        CHECK(jpq_save(&g, d) == 0);
        memset(&l, 0, sizeof l); jpq_attach(&l, &db, &hist);
        CHECK(jpq_load(&l, d) < 0);
    }

    /* a coherent save still loads cleanly */
    snprintf(dir, sizeof dir, "%s/pv_ok", tmp); mkdir(dir, 0700);
    fresh_game(2, NULL);
    CHECK(jpq_save(&g, dir) == 0);
    memset(&l, 0, sizeof l); jpq_attach(&l, &db, &hist);
    CHECK(jpq_load(&l, dir) == 0);
}

static void test_snapshot_final_dd_phases(void) {
    int32_t fc = -1;
    for (uint32_t i = 0; i < db.cat_count; i++)
        if (db.cats[i].round == JPQ_ROUND_FINAL) { fc = (int32_t)db.cats[i].first_clue; break; }
    CHECK(fc >= 0);

    fresh_game(2, NULL);
    g.round = JPQ_ROUND_FINAL;
    g.phase = JPQ_PHASE_FINAL_WAGER;
    g.final_clue = fc;
    char *js = NULL; size_t jn = 0;
    CHECK(jpq_snapshot_json(&g, JPQ_PUBLIC, &js, &jn) == 0);
    struct jval *root = json_parse(js, jn);
    CHECK(jget(jget(root, "final"), "category")->t == J_STR);
    CHECK(jget(jget(root, "final"), "text")->t == J_NULL);
    CHECK(jget(jget(root, "final"), "response")->t == J_NULL);
    jfree(root); free(js);
    /* the host also only sees the text once wagering has closed */
    js = NULL;
    CHECK(jpq_snapshot_json(&g, JPQ_HOST, &js, &jn) == 0);
    root = json_parse(js, jn);
    CHECK(jget(jget(root, "final"), "text")->t == J_NULL);
    jfree(root); free(js);

    g.phase = JPQ_PHASE_FINAL_CLUE;
    js = NULL;
    CHECK(jpq_snapshot_json(&g, JPQ_PUBLIC, &js, &jn) == 0);
    root = json_parse(js, jn);
    CHECK(jget(jget(root, "final"), "text")->t == J_STR);
    jfree(root); free(js);

    /* JPQ_PHASE_REVEAL (11) is numerically after the Final phases: an explicit
     * phase check must still hide the Final clue there */
    fresh_game(2, NULL);
    g.round = JPQ_ROUND_FINAL;
    g.phase = JPQ_PHASE_REVEAL;
    g.final_clue = fc;
    js = NULL;
    CHECK(jpq_snapshot_json(&g, JPQ_PUBLIC, &js, &jn) == 0);
    root = json_parse(js, jn);
    CHECK(jget(root, "final")->t == J_NULL);
    jfree(root); free(js);

    /* the DD correct response never leaks to players before the reveal */
    fresh_game(2, NULL);
    int col, row; find_cell(&g.board_j, 1, &col, &row);
    CHECK(apply(0, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_DD_WAGER);
    js = NULL;
    CHECK(jpq_snapshot_json(&g, 1, &js, &jn) == 0);
    root = json_parse(js, jn);
    CHECK(jget(jget(root, "clue"), "response")->t == J_NULL);
    jfree(root); free(js);
    js = NULL;
    CHECK(jpq_snapshot_json(&g, 0, &js, &jn) == 0);
    root = json_parse(js, jn);
    CHECK(jget(jget(root, "clue"), "response")->t == J_NULL);
    jfree(root); free(js);
    /* host response is acceptable */
    js = NULL;
    CHECK(jpq_snapshot_json(&g, JPQ_HOST, &js, &jn) == 0);
    root = json_parse(js, jn);
    CHECK(jget(jget(root, "clue"), "response")->t == J_STR);
    jfree(root); free(js);
    /* still hidden to the selector after the wager, before the reveal */
    CHECK(apply(0, JPQ_ACT_WAGER, 0, g.cfg.min_dd_wager, "") == 0);
    CHECK(g.phase == JPQ_PHASE_DD_CLUE);
    js = NULL;
    CHECK(jpq_snapshot_json(&g, 0, &js, &jn) == 0);
    root = json_parse(js, jn);
    CHECK(jget(jget(root, "clue"), "response")->t == J_NULL);
    jfree(root); free(js);
}

static void test_persistence_lobby_recovery(const char *tmp) {
    char dir[256];
    JpqGame l;

    /* A clean lobby (players joined, no game running) resumes unpaused so the
     * host can START and players can READY after a module upgrade or reboot;
     * any interrupted two-step confirmation is dropped and the epoch, revision,
     * sequences, ids and scores are preserved exactly. A real loadable lobby is
     * a restarted game: boards are built and round is J. */
    snprintf(dir, sizeof dir, "%s/lobby_ok", tmp); mkdir(dir, 0700);
    fresh_game(2, NULL);
    CHECK(apply(JPQ_HOST, JPQ_ACT_RESTART, 0, 0, "") == 0);
    CHECK(apply(JPQ_HOST, JPQ_ACT_RESTART, 0, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_LOBBY);
    CHECK(jpq_set_ready(&g, 0, 1) == 0);
    CHECK(jpq_set_ready(&g, 1, 1) == 0);
    g.players[0].score = 700;                     /* must survive exactly */
    g.quit_confirm = 1; g.restart_confirm = 1;   /* interrupted confirmations */
    g.act_is_dd = 1;                              /* stale modifier, no clue */
    g.epoch = 0xABCDEF01u; g.revision = 7;
    g.last_seq[0] = 11; g.last_seq[1] = 12;
    uint32_t id0 = g.players[0].id, id1 = g.players[1].id;
    CHECK(jpq_save(&g, dir) == 0);
    memset(&l, 0, sizeof l); jpq_attach(&l, &db, &hist);
    CHECK(jpq_load(&l, dir) == 0);
    CHECK(l.phase == JPQ_PHASE_LOBBY);
    CHECK(l.paused == 0);                        /* idle lobby resumes live */
    CHECK(l.quit_confirm == 0 && l.restart_confirm == 0);
    CHECK(l.timer_deadline_ms == 0);
    CHECK(l.timer_kind == JPQ_TIMER_NONE);
    CHECK(l.epoch == 0xABCDEF01u);
    CHECK(l.revision == 7);
    CHECK(l.last_seq[0] == 11 && l.last_seq[1] == 12);
    CHECK(l.player_count == 2);
    CHECK(l.players[0].id == id0 && l.players[1].id == id1);
    CHECK(l.players[0].score == 700);
    CHECK(l.players[0].ready == 1);
    CHECK(l.players[1].ready == 1);
    CHECK(!memcmp(&l.board_j, &g.board_j, sizeof g.board_j));
    /* START from the resumed lobby succeeds with a fresh sequence. */
    JpqAction a; memset(&a, 0, sizeof a);
    a.seq = 50; a.epoch = l.epoch; a.revision = l.revision; a.actor = JPQ_HOST; a.type = JPQ_ACT_START;
    CHECK(jpq_apply(&l, &a, NOW) == 0);
    CHECK(l.phase == JPQ_PHASE_BOARD);

    /* A saved lobby with an inconsistent (leftover) timer fails closed: it is
     * not trusted to auto-resume and stays paused instead. */
    char dir2[256];
    snprintf(dir2, sizeof dir2, "%s/lobby_badtimer", tmp); mkdir(dir2, 0700);
    fresh_game(2, NULL);
    CHECK(apply(JPQ_HOST, JPQ_ACT_RESTART, 0, 0, "") == 0);
    CHECK(apply(JPQ_HOST, JPQ_ACT_RESTART, 0, 0, "") == 0);
    CHECK(g.phase == JPQ_PHASE_LOBBY);
    g.timer_kind = JPQ_TIMER_READ;       /* impossible in a real lobby */
    g.timer_remaining_ms = 5000;
    CHECK(jpq_save(&g, dir2) == 0);
    memset(&l, 0, sizeof l); jpq_attach(&l, &db, &hist);
    CHECK(jpq_load(&l, dir2) == 0);
    CHECK(l.phase == JPQ_PHASE_LOBBY);
    CHECK(l.paused == 1);                /* fail closed, no auto-resume */
    CHECK(l.timer_deadline_ms == 0);
    CHECK(l.player_count == 2);

    /* An in-progress phase still resumes safely paused with its remainder and
     * a dropped confirmation. */
    char dir3[256];
    snprintf(dir3, sizeof dir3, "%s/inprogress", tmp); mkdir(dir3, 0700);
    fresh_game(2, NULL);
    int col, row; find_cell(&g.board_j, 0, &col, &row);
    CHECK(apply(JPQ_HOST, JPQ_ACT_SELECT_CLUE, col * JPQ_ROWS + row, 0, "") == 0);
    uint64_t t0 = NOW;
    NOW = t0 + g.cfg.read_ms / 3;
    CHECK(apply(JPQ_HOST, JPQ_ACT_PAUSE, 0, 0, "") == 0);
    uint64_t rem = g.timer_remaining_ms;
    CHECK(apply(JPQ_HOST, JPQ_ACT_QUIT, 0, 0, "") == 0);
    CHECK(g.quit_confirm == 1);
    CHECK(jpq_save(&g, dir3) == 0);
    memset(&l, 0, sizeof l); jpq_attach(&l, &db, &hist);
    CHECK(jpq_load(&l, dir3) == 0);
    CHECK(l.phase == JPQ_PHASE_CLUE);
    CHECK(l.paused == 1);
    CHECK(l.quit_confirm == 0);
    CHECK(l.timer_remaining_ms == rem);
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s demo.jdb fixtures.jdb tmpdir large.jdb gap.jdb\n", argv[0]);
        return 2;
    }
    if (jpq_db_load(&db, argv[1])) {
        fprintf(stderr, "cannot load %s\n", argv[1]);
        return 2;
    }
    if (jpq_history_init(&hist, db.clue_count)) return 2;
    if (jpq_db_load(&fixdb, argv[2])) {
        fprintf(stderr, "cannot load %s\n", argv[2]);
        return 2;
    }
    if (jpq_history_init(&fixhist, fixdb.clue_count)) return 2;
    if (jpq_db_load(&bigdb, argv[4])) {
        fprintf(stderr, "cannot load %s\n", argv[4]);
        return 2;
    }
    if (jpq_history_init(&bighist, bigdb.clue_count)) return 2;
    if (jpq_db_load(&gapdb, argv[5])) {
        fprintf(stderr, "cannot load %s\n", argv[5]);
        return 2;
    }
    if (jpq_history_init(&gaphist, gapdb.clue_count)) return 2;
    CHECK(db.cat_count >= 27 && db.clue_count >= 123);
    CHECK(bigdb.cat_count > 127 && bigdb.clue_count >= 1000);

    test_db_load(argv[2]);
    test_selection();
    test_history();
    test_filters();
    test_lobby();
    test_full_game();
    test_dd_constraints();
    test_dd_timeout();
    test_negative_scores();
    test_rebuzz();
    test_timers();
    test_pause_resume();
    test_replay_protection();
    test_final_private();
    test_ties_tiebreak();
    test_host_actions();
    test_persistence(argv[3]);
    test_snapshot();
    test_authentic_values();
    test_early_buzz();
    test_seq_wide();
    test_invalid_no_mutation();
    test_snapshot_redaction();
    test_episode_archive();
    test_large_indices(argv[3]);
    test_final_history_preserved();
    test_filter_at_start();
    test_selector_control();
    test_inactive_actors();
    test_round_scoped_recycling();
    test_episode_mode_freshness();
    test_explicit_episode_fail();
    test_episode_mode_requires_complete();
    test_persist_validation(argv[3]);
    test_persistence_lobby_recovery(argv[3]);
    test_snapshot_final_dd_phases();

    jpq_history_free(&hist);
    jpq_db_free(&db);
    jpq_history_free(&fixhist);
    jpq_db_free(&fixdb);
    jpq_history_free(&bighist);
    jpq_db_free(&bigdb);
    jpq_history_free(&gaphist);
    jpq_db_free(&gapdb);
    if (failures) {
        fprintf(stderr, "jeopardy: %d/%d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("jeopardy: %d checks passed (rules, dd, finals, timers, persistence, importer, redaction, archive)\n", checks);
    return 0;
}
