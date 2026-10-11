/*
 * Atomic, versioned, endian-independent persistence.
 *
 * Saves are written with rb_atomic() (a .next file, file fsync, rename and
 * parent directory fsync) so a crash never leaves a torn record. Every field is
 * encoded little-endian by hand, so the same file is valid on the big-endian
 * receiver and on a little-endian host. The monotonic deadline is deliberately
 * never written: only the remaining duration and the paused flag survive. A
 * reloaded in-progress game always resumes paused; a clean lobby (no clue,
 * no timer) resumes unpaused because no game is actually running.
 *
 * Corrupt or incompatible saves are preserved as .invalid-NN for recovery and
 * a fresh game is started, matching the project's storage conventions.
 */
#include "jeopardy.h"
#include "../../../reboxd/core.h"

#define JPQ_SNAP_MAGIC "JEPQSNP1"
#define JPQ_SNAP_VERSION 2u
#define JPQ_HIST_MAGIC "JEPHIST1"
#define JPQ_HIST_VERSION 1u
#define JPQ_SNAP_MAX (16u * 1024u)
#define JPQ_HIST_MAX (8u * 1024u * 1024u)

struct cur { unsigned char *p; size_t len, cap; int err; };

static void c_need(struct cur *c, size_t n) {
    if (c->err) return;
    if (c->len + n > c->cap) { c->err = 1; return; }
}
static void c_u8(struct cur *c, uint8_t v) { c_need(c, 1); if (!c->err) c->p[c->len++] = v; }
static void c_u32(struct cur *c, uint32_t v) { c_need(c, 4); if (!c->err) for (int i = 0; i < 4; i++) c->p[c->len++] = (v >> (8 * i)) & 0xff; }
static void c_i32(struct cur *c, int32_t v) { c_u32(c, (uint32_t)v); }
static void c_u64(struct cur *c, uint64_t v) { c_need(c, 8); if (!c->err) for (int i = 0; i < 8; i++) c->p[c->len++] = (v >> (8 * i)) & 0xff; }
static void c_bytes(struct cur *c, const void *src, size_t n) { c_need(c, n); if (!c->err) { memcpy(c->p + c->len, src, n); c->len += n; } }

static uint8_t d_u8(struct cur *c) { if (c->len + 1 > c->cap) { c->err = 1; return 0; } return c->p[c->len++]; }
static uint32_t d_u32(struct cur *c) { if (c->len + 4 > c->cap) { c->err = 1; return 0; } uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)c->p[c->len + i] << (8 * i); c->len += 4; return v; }
static int32_t d_i32(struct cur *c) { return (int32_t)d_u32(c); }
static uint64_t d_u64(struct cur *c) { if (c->len + 8 > c->cap) { c->err = 1; return 0; } uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)c->p[c->len + i] << (8 * i); c->len += 8; return v; }
static void d_bytes(struct cur *c, void *dst, size_t n) { if (c->len + n > c->cap) { c->err = 1; return; } memcpy(dst, c->p + c->len, n); c->len += n; }

static uint32_t fnv(const unsigned char *p, size_t n) {
    uint32_t v = 2166136261u;
    for (size_t i = 0; i < n; i++) v = (v ^ p[i]) * 16777619u;
    return v;
}

static void enc_board(struct cur *c, const JpqBoard *b) {
    c_u8(c, b->round);
    for (int i = 0; i < JPQ_COLS; i++) c_i32(c, b->cat[i]);
    for (int i = 0; i < JPQ_COLS; i++) for (int j = 0; j < JPQ_ROWS; j++) c_i32(c, b->clue[i][j]);
    for (int i = 0; i < JPQ_COLS; i++) for (int j = 0; j < JPQ_ROWS; j++) c_i32(c, b->value[i][j]);
    for (int i = 0; i < JPQ_COLS; i++) for (int j = 0; j < JPQ_ROWS; j++) c_u8(c, b->used[i][j]);
    c_u8(c, b->dd_count);
    for (int i = 0; i < JPQ_MAX_DD; i++) c_u8(c, b->dd_col[i]);
    for (int i = 0; i < JPQ_MAX_DD; i++) c_u8(c, b->dd_row[i]);
    c_i32(c, b->final_clue);
}
static void dec_board(struct cur *c, JpqBoard *b) {
    b->round = d_u8(c);
    for (int i = 0; i < JPQ_COLS; i++) b->cat[i] = d_i32(c);
    for (int i = 0; i < JPQ_COLS; i++) for (int j = 0; j < JPQ_ROWS; j++) b->clue[i][j] = d_i32(c);
    for (int i = 0; i < JPQ_COLS; i++) for (int j = 0; j < JPQ_ROWS; j++) b->value[i][j] = d_i32(c);
    for (int i = 0; i < JPQ_COLS; i++) for (int j = 0; j < JPQ_ROWS; j++) b->used[i][j] = d_u8(c);
    b->dd_count = d_u8(c);
    for (int i = 0; i < JPQ_MAX_DD; i++) b->dd_col[i] = d_u8(c);
    for (int i = 0; i < JPQ_MAX_DD; i++) b->dd_row[i] = d_u8(c);
    b->final_clue = d_i32(c);
}

static void enc_cfg(struct cur *c, const JpqConfig *f) {
    c_u32(c, f->read_ms); c_u32(c, f->buzz_ms); c_u32(c, f->answer_ms);
    c_u32(c, f->dd_wager_ms); c_u32(c, f->dd_answer_ms);
    c_u32(c, f->final_wager_ms); c_u32(c, f->final_answer_ms); c_u32(c, f->tiebreak_answer_ms);
    c_u8(c, f->allow_negative); c_u8(c, f->lockout_rebuzz); c_u8(c, f->random_categories);
    c_u8(c, f->allow_tiebreak); c_u8(c, f->dd_count_j); c_u8(c, f->dd_count_dj); c_u8(c, f->min_dd_wager);
}
static void dec_cfg(struct cur *c, JpqConfig *f) {
    f->read_ms = d_u32(c); f->buzz_ms = d_u32(c); f->answer_ms = d_u32(c);
    f->dd_wager_ms = d_u32(c); f->dd_answer_ms = d_u32(c);
    f->final_wager_ms = d_u32(c); f->final_answer_ms = d_u32(c); f->tiebreak_answer_ms = d_u32(c);
    f->allow_negative = d_u8(c); f->lockout_rebuzz = d_u8(c); f->random_categories = d_u8(c);
    f->allow_tiebreak = d_u8(c); f->dd_count_j = d_u8(c); f->dd_count_dj = d_u8(c); f->min_dd_wager = d_u8(c);
}

static void enc_snapshot(struct cur *c, const JpqGame *g) {
    c_u32(c, g->epoch); c_u32(c, g->revision); c_u64(c, g->created_ms);
    c_u8(c, g->phase); c_u8(c, g->paused); c_u8(c, g->round); c_u8(c, g->player_count);
    c_u8(c, g->host_player); c_u8(c, g->selector);
    c_u8(c, g->tiebreak_active); c_u8(c, g->quit_confirm); c_u8(c, g->restart_confirm); c_u8(c, g->quit);
    c_i32(c, g->act_col); c_i32(c, g->act_row);
    c_u8(c, g->act_is_dd); c_u8(c, g->dd_selector);
    c_i32(c, g->act_value);
    c_i32(c, g->final_clue);
    c_i32(c, g->tiebreak_winner);
    c_u8(c, g->filter_set);
    c_u32(c, g->filter.year_min); c_u32(c, g->filter.year_max);
    c_i32(c, g->filter.difficulty_min); c_i32(c, g->filter.difficulty_max);
    c_i32(c, g->filter.category); c_u32(c, g->filter.episode);
    c_u8(c, g->timer_kind); c_u64(c, g->timer_remaining_ms); c_u64(c, g->buzz_remaining_ms);
    c_u32(c, g->rng);
    enc_cfg(c, &g->cfg);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) {
        c_u32(c, g->players[p].id);
        c_bytes(c, g->players[p].name, JPQ_NAME_MAX);
        c_i32(c, g->players[p].score);
        c_u8(c, g->players[p].ready); c_u8(c, g->players[p].connected); c_u8(c, g->players[p].active);
    }
    enc_board(c, &g->board_j);
    enc_board(c, &g->board_dj);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) c_u8(c, g->excluded[p]);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) c_u8(c, g->answered[p]);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) c_u8(c, g->judged[p]);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) c_u8(c, g->correct[p]);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) c_u8(c, g->eligible[p]);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) c_i32(c, g->wager[p]);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) c_u8(c, g->wager_set[p]);
    c_u8(c, (uint8_t)g->buzzed); c_u8(c, g->judge_cursor);
    for (int p = 0; p < 8; p++) c_u32(c, g->last_seq[p]);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) c_bytes(c, g->answer[p], JPQ_ANSWER_MAX);
}

static void dec_snapshot(struct cur *c, JpqGame *g) {
    g->epoch = d_u32(c); g->revision = d_u32(c); g->created_ms = d_u64(c);
    g->phase = d_u8(c); g->paused = d_u8(c); g->round = d_u8(c); g->player_count = d_u8(c);
    g->host_player = d_u8(c); g->selector = d_u8(c);
    g->tiebreak_active = d_u8(c); g->quit_confirm = d_u8(c); g->restart_confirm = d_u8(c); g->quit = d_u8(c);
    g->act_col = d_i32(c); g->act_row = d_i32(c);
    g->act_is_dd = d_u8(c); g->dd_selector = d_u8(c);
    g->act_value = d_i32(c);
    g->final_clue = d_i32(c);
    g->tiebreak_winner = d_i32(c);
    g->filter_set = d_u8(c);
    g->filter.year_min = d_u32(c); g->filter.year_max = d_u32(c);
    g->filter.difficulty_min = d_i32(c); g->filter.difficulty_max = d_i32(c);
    g->filter.category = d_i32(c); g->filter.episode = d_u32(c);
    g->timer_kind = d_u8(c); g->timer_remaining_ms = d_u64(c); g->buzz_remaining_ms = d_u64(c);
    g->rng = d_u32(c);
    dec_cfg(c, &g->cfg);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) {
        g->players[p].id = d_u32(c);
        d_bytes(c, g->players[p].name, JPQ_NAME_MAX);
        g->players[p].name[JPQ_NAME_MAX - 1] = 0;
        g->players[p].score = d_i32(c);
        g->players[p].ready = d_u8(c); g->players[p].connected = d_u8(c); g->players[p].active = d_u8(c);
    }
    dec_board(c, &g->board_j);
    dec_board(c, &g->board_dj);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) g->excluded[p] = d_u8(c);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) g->answered[p] = d_u8(c);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) g->judged[p] = d_u8(c);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) g->correct[p] = d_u8(c);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) g->eligible[p] = d_u8(c);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) g->wager[p] = d_i32(c);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) g->wager_set[p] = d_u8(c);
    g->buzzed = (int8_t)d_u8(c); g->judge_cursor = d_u8(c);
    for (int p = 0; p < 8; p++) g->last_seq[p] = d_u32(c);
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) d_bytes(c, g->answer[p], JPQ_ANSWER_MAX);
}

static int valid_name(const char *n) {
    return n && memchr(n, 0, JPQ_NAME_MAX) != NULL;
}

/* A board is coherent only if every non-empty category/clue index is inside
 * the attached database, clues belong to their column's category, every
 * daily-double cell is a real clue cell and all used flags are 0/1. */
static int valid_board(const JpqGame *g, const JpqBoard *b) {
    const JpqDb *db = g->db;
    if (b->round < JPQ_ROUND_J || b->round > JPQ_ROUND_TIEBREAK) return 0;
    if (b->dd_count > JPQ_MAX_DD) return 0;
    for (int c = 0; c < JPQ_COLS; c++) {
        int32_t cat = b->cat[c];
        if (cat < -1) return 0;
        if (cat >= 0 && (!db || (uint32_t)cat >= db->cat_count)) return 0;
        for (int r = 0; r < JPQ_ROWS; r++) {
            int32_t cl = b->clue[c][r];
            if (cl < -1) return 0;
            if (cl >= 0) {
                if (!db || (uint32_t)cl >= db->clue_count) return 0;
                if (cat >= 0 && (int32_t)db->clues[cl].category != cat) return 0;
            } else if (b->value[c][r] != 0) {
                return 0; /* empty cell cannot carry a value */
            }
            if (b->value[c][r] < 0) return 0;
            if (b->used[c][r] > 1) return 0;
        }
    }
    for (uint8_t i = 0; i < b->dd_count; i++) {
        if (b->dd_col[i] >= JPQ_COLS || b->dd_row[i] >= JPQ_ROWS) return 0;
        if (b->clue[b->dd_col[i]][b->dd_row[i]] < 0) return 0;
    }
    if (b->final_clue < -1) return 0;
    if (b->final_clue >= 0 && (!db || (uint32_t)b->final_clue >= db->clue_count)) return 0;
    return 1;
}

static int game_state_valid(const JpqGame *g) {
    if (g->phase > JPQ_PHASE_REVEAL) return 0;
    if (g->player_count > JPQ_MAX_PLAYERS) return 0;
    if (g->round < JPQ_ROUND_J || g->round > JPQ_ROUND_TIEBREAK) return 0;
    /* Indices can only be checked against the database the service attached;
     * a save from a different database must fail rather than read wild. */
    if (!g->db) return 0;
    if (g->timer_kind > JPQ_TIMER_TIEBREAK_ANSWER) return 0;
    if (g->selector >= g->player_count) return 0;
    if (g->host_player >= JPQ_MAX_PLAYERS) return 0;
    if (g->dd_selector != JPQ_NOBODY && g->dd_selector >= g->player_count) return 0;
    if ((uint8_t)g->buzzed != JPQ_NOBODY && (uint8_t)g->buzzed >= g->player_count) return 0;
    if (g->judge_cursor != JPQ_NOBODY && g->judge_cursor >= g->player_count) return 0;
    if (g->tiebreak_winner < -1 || g->tiebreak_winner >= (int32_t)g->player_count) return 0;
    if (g->act_col < -1 || g->act_col >= JPQ_COLS) return 0;
    if (g->act_row < -1 || g->act_row >= JPQ_ROWS) return 0;
    if (g->act_value < 0) return 0;
    for (int p = 0; p < JPQ_MAX_PLAYERS; p++) {
        if (!valid_name(g->players[p].name)) return 0;
        if (g->players[p].ready > 1 || g->players[p].connected > 1 || g->players[p].active > 1) return 0;
        if (g->excluded[p] > 1 || g->answered[p] > 1 || g->judged[p] > 1 || g->correct[p] > 1) return 0;
        if (g->eligible[p] > 1 || g->wager_set[p] > 1) return 0;
    }
    if (!valid_board(g, &g->board_j) || !valid_board(g, &g->board_dj)) return 0;
    return 1;
}

/* A saved game may resume unpaused only when it represents an idle lobby:
 * the phase is LOBBY, no clue is attached and no timer is running or left
 * over. A lobby whose persisted timer is inconsistent (a stray kind or
 * remainder) is not trusted to resume and stays safely paused. Anything in
 * progress is never auto-resumed. (The pre-built Final pool and a stale
 * daily-double modifier without an attached clue are not an active game: a
 * restarted/reset lobby legitimately carries them.) */
static int recovery_is_idle_lobby(const JpqGame *g) {
    if (g->phase != JPQ_PHASE_LOBBY) return 0;
    if (g->timer_kind != JPQ_TIMER_NONE) return 0;
    if (g->timer_remaining_ms != 0 || g->buzz_remaining_ms != 0) return 0;
    if (g->act_col != -1 || g->act_row != -1) return 0;
    return 1;
}

int jpq_save(const JpqGame *g, const char *dir) {
    if (!g || !dir) return -1;
    char path[REBOX_PATH_MAX];
    if (snprintf(path, sizeof path, "%s/jeopardy-v1.bin", dir) >= (int)sizeof path) return -1;
    unsigned char *buf = calloc(1, JPQ_SNAP_MAX);
    if (!buf) return -1;
    struct cur c = {buf + 16, 0, JPQ_SNAP_MAX - 16, 0};
    enc_snapshot(&c, g);
    if (c.err) { free(buf); return -1; }
    memcpy(buf, JPQ_SNAP_MAGIC, 8);
    /* version and payload length */
    for (int i = 0; i < 4; i++) { buf[8 + i] = (JPQ_SNAP_VERSION >> (8 * i)) & 0xff; buf[12 + i] = ((uint32_t)c.len >> (8 * i)) & 0xff; }
    uint32_t sum = fnv(buf, 16 + c.len);
    for (int i = 0; i < 4; i++) buf[16 + c.len + i] = (sum >> (8 * i)) & 0xff;
    int rc = rb_atomic(path, buf, 20 + c.len);
    free(buf);
    return rc;
}

int jpq_load(JpqGame *g, const char *dir) {
    if (!g || !dir) return -1;
    char path[REBOX_PATH_MAX];
    if (snprintf(path, sizeof path, "%s/jeopardy-v1.bin", dir) >= (int)sizeof path) return -1;
    if (access(path, F_OK)) return -1; /* no save yet: fresh start */
    size_t len = 0;
    unsigned char *raw = (unsigned char *)rb_read(path, JPQ_SNAP_MAX, &len);
    int ok = 0;
    if (raw && len >= 20 && !memcmp(raw, JPQ_SNAP_MAGIC, 8)) {
        uint32_t version = (uint32_t)raw[8] | ((uint32_t)raw[9] << 8) | ((uint32_t)raw[10] << 16) | ((uint32_t)raw[11] << 24);
        uint32_t payload = (uint32_t)raw[12] | ((uint32_t)raw[13] << 8) | ((uint32_t)raw[14] << 16) | ((uint32_t)raw[15] << 24);
        if (version == JPQ_SNAP_VERSION && payload + 20 == len) {
            uint32_t sum = (uint32_t)raw[16 + payload] | ((uint32_t)raw[17 + payload] << 8) |
                           ((uint32_t)raw[18 + payload] << 16) | ((uint32_t)raw[19 + payload] << 24);
            if (sum == fnv(raw, 16 + payload)) {
                JpqDb *db = g->db; JpqHistory *hist = g->history;
                memset(g, 0, sizeof *g);
                g->db = db; g->history = hist;
                struct cur c = {raw + 16, 0, payload, 0};
                dec_snapshot(&c, g);
                if (!c.err && game_state_valid(g)) {
                    /* never trust a persisted deadline */
                    g->timer_deadline_ms = 0;
                    /* An interrupted two-step quit/restart confirmation is
                     * transient UI state: it must never survive a reload and
                     * silently commit on the next keypress. */
                    g->quit_confirm = 0;
                    g->restart_confirm = 0;
                    /* A clean lobby is not a game in progress: resume it
                     * unpaused so ordinary players can READY and the host can
                     * START after a module upgrade or reboot. Every other
                     * phase - and a lobby with an inconsistent saved timer -
                     * stays paused on its saved remainder. */
                    g->paused = recovery_is_idle_lobby(g) ? 0 : 1;
                    ok = 1;
                }
            }
        }
    }
    free(raw);
    if (ok) return 0;
    /* preserve the unrecognized save for recovery */
    for (int i = 0; i < 100; i++) {
        char backup[REBOX_PATH_MAX];
        if (snprintf(backup, sizeof backup, "%s/jeopardy-v1.invalid-%02d.bin", dir, i) >= (int)sizeof backup) return -2;
        if (!access(backup, F_OK)) continue;
        if (!rename(path, backup)) return -1;
        return -2;
    }
    return -2;
}

/* ------------------------------------------------------------------ */
/* History persistence                                                 */
/* ------------------------------------------------------------------ */

int jpq_history_save(const JpqHistory *h, const char *path) {
    if (!h || !path || !h->bits) return -1;
    size_t bytes = (h->count + 7) / 8;
    size_t total = 12 + bytes + 4;
    unsigned char *buf = calloc(1, total);
    if (!buf) return -1;
    memcpy(buf, JPQ_HIST_MAGIC, 8);
    uint32_t count = h->count;
    for (int i = 0; i < 4; i++) buf[8 + i] = (count >> (8 * i)) & 0xff;
    memcpy(buf + 12, h->bits, bytes);
    uint32_t sum = fnv(buf, 12 + bytes);
    for (int i = 0; i < 4; i++) buf[12 + bytes + i] = (sum >> (8 * i)) & 0xff;
    int rc = rb_atomic(path, buf, total);
    free(buf);
    return rc;
}

int jpq_history_load(JpqHistory *h, const char *path) {
    if (!h || !path) return -1;
    if (access(path, F_OK)) return -1;
    size_t len = 0;
    unsigned char *raw = (unsigned char *)rb_read(path, JPQ_HIST_MAX, &len);
    int ok = 0;
    if (raw && len >= 16 && !memcmp(raw, JPQ_HIST_MAGIC, 8)) {
        uint32_t count = (uint32_t)raw[8] | ((uint32_t)raw[9] << 8) | ((uint32_t)raw[10] << 16) | ((uint32_t)raw[11] << 24);
        size_t bytes = (count + 7) / 8;
        if (12 + bytes + 4 == len) {
            uint32_t sum = (uint32_t)raw[12 + bytes] | ((uint32_t)raw[13 + bytes] << 8) |
                           ((uint32_t)raw[14 + bytes] << 16) | ((uint32_t)raw[15 + bytes] << 24);
            if (sum == fnv(raw, 12 + bytes)) {
                if (jpq_history_init(h, count)) { free(raw); return -2; }
                memcpy(h->bits, raw + 12, bytes);
                ok = 1;
            }
        }
    }
    free(raw);
    return ok ? 0 : -2;
}
