/*
 * Authoritative Jeopardy rules and state machine.
 *
 * All timing decisions use the server-supplied monotonic millisecond clock.
 * Actions carry epoch/revision/sequence so duplicate, stale or replayed
 * commands are rejected before any state changes. Timers persist only their
 * remaining duration; a monotonic deadline is never written to disk.
 */
#include "jeopardy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void jpq_config_default(JpqConfig *c) {
    memset(c, 0, sizeof *c);
    c->read_ms = 4000;
    c->buzz_ms = 6000;
    c->answer_ms = 8000;
    c->dd_wager_ms = 20000;
    c->dd_answer_ms = 20000;
    c->final_wager_ms = 30000;
    c->final_answer_ms = 30000;
    c->tiebreak_answer_ms = 8000;
    c->allow_negative = 1;
    c->lockout_rebuzz = 1;
    c->random_categories = 1;
    c->allow_tiebreak = 1;
    c->dd_count_j = 1;
    c->dd_count_dj = 2;
    c->min_dd_wager = 5;
}

static int reject(JpqGame *g, const char *msg) {
    snprintf(g->err, sizeof g->err, "%s", msg);
    return -1;
}

static int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }

/* ------------------------------------------------------------------ */
/* Timer primitives                                                    */
/* ------------------------------------------------------------------ */

static void clear_timer(JpqGame *g) {
    g->timer_kind = JPQ_TIMER_NONE;
    g->timer_remaining_ms = 0;
    g->timer_deadline_ms = 0;
}

static void start_timer(JpqGame *g, uint8_t kind, uint32_t ms, uint64_t now) {
    g->timer_kind = kind;
    g->timer_remaining_ms = ms;
    g->timer_deadline_ms = (ms && !g->paused) ? now + ms : 0;
}

static void pause_timer(JpqGame *g, uint64_t now) {
    if (g->timer_deadline_ms) {
        g->timer_remaining_ms = g->timer_deadline_ms > now ? g->timer_deadline_ms - now : 0;
        g->timer_deadline_ms = 0;
    }
}
static void resume_timer(JpqGame *g, uint64_t now) {
    if (g->timer_kind != JPQ_TIMER_NONE && !g->timer_deadline_ms && g->timer_remaining_ms)
        g->timer_deadline_ms = now + g->timer_remaining_ms;
}

static int timer_expired(const JpqGame *g, uint64_t now) {
    return g->timer_kind != JPQ_TIMER_NONE && g->timer_deadline_ms && now >= g->timer_deadline_ms;
}

/* ------------------------------------------------------------------ */
/* Board helpers                                                       */
/* ------------------------------------------------------------------ */

static JpqBoard *cur_board(JpqGame *g) {
    return g->round == JPQ_ROUND_DJ ? &g->board_dj : &g->board_j;
}
static uint32_t round_max_value(const JpqBoard *b) {
    uint32_t m = 0;
    for (int c = 0; c < JPQ_COLS; c++)
        for (int r = 0; r < JPQ_ROWS; r++)
            if (b->value[c][r] > 0 && (uint32_t)b->value[c][r] > m) m = (uint32_t)b->value[c][r];
    return m;
}
static int board_complete(const JpqBoard *b) {
    for (int c = 0; c < JPQ_COLS; c++)
        for (int r = 0; r < JPQ_ROWS; r++)
            if (!b->used[c][r]) return 0;
    return 1;
}
static int is_dd(const JpqBoard *b, int col, int row) {
    for (uint8_t i = 0; i < b->dd_count; i++)
        if (b->dd_col[i] == col && b->dd_row[i] == row) return 1;
    return 0;
}

static void reset_clue_state(JpqGame *g) {
    memset(g->excluded, 0, sizeof g->excluded);
    memset(g->answered, 0, sizeof g->answered);
    memset(g->judged, 0, sizeof g->judged);
    memset(g->correct, 0, sizeof g->correct);
    memset(g->answer, 0, sizeof g->answer);
    g->buzzed = (int8_t)JPQ_NOBODY;
    g->judge_cursor = 0;
    g->buzz_remaining_ms = 0;
}

static void add_score(JpqGame *g, uint8_t p, int32_t delta) {
    int64_t s = (int64_t)g->players[p].score + delta;
    if (!g->cfg.allow_negative && s < 0) s = 0;
    if (s > 2147483647LL) s = 2147483647LL;
    if (s < -2147483648LL) s = -2147483648LL;
    g->players[p].score = (int32_t)s;
}

static uint8_t first_eligible(const JpqGame *g, uint8_t from) {
    for (uint8_t p = from; p < JPQ_MAX_PLAYERS; p++)
        if (g->eligible[p] && g->players[p].active) return p;
    return JPQ_NOBODY;
}

static int buzzable(const JpqGame *g, uint8_t p) {
    return g->players[p].active && !g->excluded[p] && !g->answered[p];
}

/* ------------------------------------------------------------------ */
/* Clue lifecycle                                                      */
/* ------------------------------------------------------------------ */

static void enter_buzz(JpqGame *g, uint64_t now, uint64_t remaining) {
    g->timer_kind = JPQ_TIMER_BUZZ;
    g->buzz_remaining_ms = remaining;
    g->timer_remaining_ms = remaining;
    g->timer_deadline_ms = (remaining && !g->paused) ? now + remaining : 0;
}

static void start_reading(JpqGame *g, uint64_t now) {
    if (g->cfg.read_ms) start_timer(g, JPQ_TIMER_READ, g->cfg.read_ms, now);
    else enter_buzz(g, now, g->cfg.buzz_ms);
}

static void enter_reveal(JpqGame *g, uint64_t now);

static void rebuzz_or_reveal(JpqGame *g, uint64_t now) {
    int any = 0;
    for (uint8_t p = 0; p < g->player_count; p++) if (buzzable(g, p)) any = 1;
    if (!any) { enter_reveal(g, now); return; }
    if (g->buzz_remaining_ms == 0) {
        if (g->cfg.buzz_ms == 0) enter_buzz(g, now, 0); /* unlimited window */
        else enter_reveal(g, now);
        return;
    }
    enter_buzz(g, now, g->buzz_remaining_ms);
}

/* Hold the resolved clue (text + correct response) on screen. The host must
 * ADVANCE to leave the reveal, so the response never disappears unshown. The
 * active clue and its judged state survive this phase. */
static void enter_reveal(JpqGame *g, uint64_t now) {
    clear_timer(g);
    if (g->round == JPQ_ROUND_TIEBREAK || g->tiebreak_active) {
        /* sudden death resolves straight to results; no monetary change */
        g->tiebreak_active = 0;
        g->phase = JPQ_PHASE_RESULTS;
        return;
    }
    g->phase = JPQ_PHASE_REVEAL;
}

static int lowest_score(const JpqGame *g) {
    int best = -1;
    for (uint8_t p = 0; p < g->player_count; p++)
        if (g->players[p].active && (best < 0 || g->players[p].score < g->players[best].score)) best = p;
    return best < 0 ? 0 : best;
}

static void begin_final(JpqGame *g, uint64_t now) {
    g->round = JPQ_ROUND_FINAL;
    uint8_t any = 0;
    for (uint8_t p = 0; p < g->player_count; p++) {
        g->eligible[p] = g->players[p].active && g->players[p].score > 0;
        g->wager[p] = 0; g->wager_set[p] = 0; g->answered[p] = 0;
        g->judged[p] = 0; g->correct[p] = 0;
        memset(g->answer[p], 0, sizeof g->answer[p]);
        if (g->eligible[p]) any = 1;
    }
    for (uint8_t p = 0; p < g->player_count; p++) if (!g->eligible[p]) g->wager_set[p] = 1;
    if (!any) { clear_timer(g); g->phase = JPQ_PHASE_RESULTS; return; }
    g->phase = JPQ_PHASE_FINAL_WAGER;
    if (g->cfg.final_wager_ms) start_timer(g, JPQ_TIMER_FINAL_WAGER, g->cfg.final_wager_ms, now);
    else clear_timer(g);
}

/* Leave the reveal phase: mark the clue used and move to the next board /
 * round / results. */
static void advance_after_reveal(JpqGame *g, uint64_t now) {
    JpqBoard *b = cur_board(g);
    if (g->act_col >= 0 && g->act_row >= 0) b->used[g->act_col][g->act_row] = 1;
    reset_clue_state(g);
    g->act_col = -1; g->act_row = -1;
    if (g->round == JPQ_ROUND_J && board_complete(&g->board_j)) {
        g->round = JPQ_ROUND_DJ;
        g->selector = (uint8_t)lowest_score(g);
        g->phase = JPQ_PHASE_BOARD;
    } else if (g->round == JPQ_ROUND_DJ && board_complete(&g->board_dj)) {
        begin_final(g, now);
    } else {
        g->phase = JPQ_PHASE_BOARD;
    }
}

/* ------------------------------------------------------------------ */
/* Action validation                                                   */
/* ------------------------------------------------------------------ */

static int actor_slot(const JpqAction *a) {
    if (a->actor == JPQ_HOST) return 6;
    if (a->actor < JPQ_MAX_PLAYERS) return a->actor;
    return -1;
}

static int check_envelope(JpqGame *g, const JpqAction *a) {
    if (a->epoch != g->epoch) return reject(g, "stale epoch");
    if (a->revision != g->revision) return reject(g, "stale revision");
    int slot = actor_slot(a);
    if (slot < 0) return reject(g, "invalid actor");
    if (a->seq <= g->last_seq[slot]) return reject(g, "replayed sequence");
    if (a->actor != JPQ_HOST && a->actor >= g->player_count) return reject(g, "unknown player");
    /* A contestant who is no longer active may not act at all. */
    if (a->actor != JPQ_HOST && !g->players[a->actor].active) return reject(g, "inactive player");
    /* A pause holds every gameplay action, but the host may still back out or
     * restart from the pause menu: CANCEL/RESTART are safe (they either just
     * disarm a confirmation or rebuild the lobby) and are host-only, so a
     * rejected player can never cancel, restart or otherwise slip past here. */
    if (g->paused && a->type != JPQ_ACT_RESUME && a->type != JPQ_ACT_QUIT && a->type != JPQ_ACT_PAUSE &&
        !(a->actor == JPQ_HOST && (a->type == JPQ_ACT_RESTART || a->type == JPQ_ACT_CANCEL)))
        return reject(g, "game paused");
    return slot;
}

static int host_only(JpqGame *g, const JpqAction *a) {
    return a->actor == JPQ_HOST ? 0 : reject(g, "host only");
}

/* ------------------------------------------------------------------ */
/* Wager bounds                                                        */
/* ------------------------------------------------------------------ */

static int32_t dd_max_wager(const JpqGame *g, uint8_t sel) {
    int32_t m = (int32_t)round_max_value(cur_board((JpqGame *)g));
    return imax(g->players[sel].score, m);
}

/* ------------------------------------------------------------------ */
/* Apply                                                               */
/* ------------------------------------------------------------------ */

int jpq_apply(JpqGame *g, const JpqAction *a, uint64_t now_ms) {
    if (!g || !a) return -1;
    g->err[0] = 0;
    int slot = check_envelope(g, a);
    if (slot < 0) return -1;

    int rc = 0;
    switch (a->type) {
    case JPQ_ACT_READY: {
        if (g->phase != JPQ_PHASE_LOBBY) { rc = reject(g, "not in lobby"); break; }
        if (a->actor == JPQ_HOST) { rc = reject(g, "players only"); break; }
        g->players[a->actor].ready = a->value ? 1 : 0;
        break;
    }
    case JPQ_ACT_START: {
        if (host_only(g, a)) { rc = -1; break; }
        if (g->phase != JPQ_PHASE_LOBBY) { rc = reject(g, "already started"); break; }
        if (g->player_count < 2 || g->player_count > JPQ_MAX_PLAYERS) { rc = reject(g, "need 2-6 players"); break; }
        for (uint8_t p = 0; p < g->player_count; p++)
            if (!g->players[p].ready) { rc = reject(g, "players not ready"); break; }
        if (rc) break;
        uint32_t seed = g->rng ? g->rng : 0xC0FFEEu;
        if (jpq_build_boards(g, seed)) { rc = reject(g, "no question pool"); break; }
        g->epoch = (g->rng ^ 0x9e3779b9u) | 1u;
        g->revision = 0;
        memset(g->last_seq, 0, sizeof g->last_seq);
        reset_clue_state(g);
        g->round = JPQ_ROUND_J;
        g->selector = 0;
        g->tiebreak_winner = -1;
        g->phase = JPQ_PHASE_BOARD;
        g->paused = 0; g->quit = 0;
        break;
    }
    case JPQ_ACT_SELECT_CLUE: {
        if (g->phase != JPQ_PHASE_BOARD) { rc = reject(g, "not on board"); break; }
        /* Only the player who currently controls the board may pick; the host
         * may select on the controller's behalf without stealing control. */
        if (a->actor != JPQ_HOST && a->actor != g->selector) { rc = reject(g, "not your selection"); break; }
        int col = (a->target / JPQ_ROWS);
        int row = (a->target % JPQ_ROWS);
        if (col < 0 || col >= JPQ_COLS || row < 0 || row >= JPQ_ROWS) { rc = reject(g, "bad clue"); break; }
        JpqBoard *b = cur_board(g);
        if (b->clue[col][row] < 0 || b->used[col][row]) { rc = reject(g, "clue unavailable"); break; }
        g->act_col = col; g->act_row = row;
        g->act_value = b->value[col][row];
        reset_clue_state(g);
        if (is_dd(b, col, row)) {
            g->act_is_dd = 1;
            g->dd_selector = g->selector;
            g->wager_set[g->dd_selector] = 0;
            g->wager[g->dd_selector] = 0;
            g->phase = JPQ_PHASE_DD_WAGER;
            if (g->cfg.dd_wager_ms) start_timer(g, JPQ_TIMER_DD_WAGER, g->cfg.dd_wager_ms, now_ms);
            else clear_timer(g);
        } else {
            g->act_is_dd = 0;
            g->phase = JPQ_PHASE_CLUE;
            start_reading(g, now_ms);
        }
        break;
    }
    case JPQ_ACT_BUZZ: {
        if (g->phase != JPQ_PHASE_CLUE && g->phase != JPQ_PHASE_TIEBREAK_CLUE) { rc = reject(g, "buzzer closed"); break; }
        if (g->timer_kind != JPQ_TIMER_BUZZ) { rc = reject(g, "buzzer closed"); break; }
        if (g->timer_deadline_ms && now_ms >= g->timer_deadline_ms) { rc = reject(g, "buzzer closed"); break; }
        if ((uint8_t)g->buzzed != JPQ_NOBODY) { rc = reject(g, "already buzzed"); break; }
        if (!buzzable(g, a->actor)) { rc = reject(g, "cannot buzz"); break; }
        g->buzzed = (int8_t)a->actor;
        uint64_t rem = g->timer_deadline_ms ? (g->timer_deadline_ms > now_ms ? g->timer_deadline_ms - now_ms : 0) : 0;
        g->buzz_remaining_ms = rem;
        uint32_t t = (g->phase == JPQ_PHASE_TIEBREAK_CLUE) ? g->cfg.tiebreak_answer_ms : g->cfg.answer_ms;
        if (t) start_timer(g, JPQ_TIMER_ANSWER, t, now_ms);
        else clear_timer(g);
        break;
    }
    case JPQ_ACT_SUBMIT: {
        if (g->phase == JPQ_PHASE_DD_CLUE) {
            if (a->actor != g->dd_selector) { rc = reject(g, "selector only"); break; }
        } else if (g->phase == JPQ_PHASE_FINAL_CLUE) {
            if (!g->eligible[a->actor]) { rc = reject(g, "not eligible"); break; }
        } else if (g->phase == JPQ_PHASE_TIEBREAK_CLUE) {
            if (!g->eligible[a->actor]) { rc = reject(g, "not eligible"); break; }
        } else { rc = reject(g, "no answer pending"); break; }
        if (g->answered[a->actor]) { rc = reject(g, "answer already submitted"); break; }
        snprintf(g->answer[a->actor], sizeof g->answer[a->actor], "%s", a->text);
        g->answered[a->actor] = 1;
        /* answer text is private; stored in the action, judged by the host */
        if (g->phase == JPQ_PHASE_FINAL_CLUE) {
            int all = 1;
            for (uint8_t p = 0; p < g->player_count; p++) if (g->eligible[p] && !g->answered[p]) all = 0;
            if (all) {
                clear_timer(g);
                g->phase = JPQ_PHASE_FINAL_JUDGE;
                g->judge_cursor = first_eligible(g, 0);
            }
        }
        break;
    }
    case JPQ_ACT_WAGER: {
        if (g->phase == JPQ_PHASE_DD_WAGER) {
            if (a->actor != g->dd_selector) { rc = reject(g, "selector only"); break; }
            if (g->wager_set[a->actor]) { rc = reject(g, "wager already set"); break; }
            int32_t maxw = dd_max_wager(g, a->actor);
            if (a->value < g->cfg.min_dd_wager) { rc = reject(g, "wager below minimum"); break; }
            if (a->value > maxw) { rc = reject(g, "wager above maximum"); break; }
            g->wager[a->actor] = a->value;
            g->wager_set[a->actor] = 1;
            g->phase = JPQ_PHASE_DD_CLUE;
            if (g->cfg.dd_answer_ms) start_timer(g, JPQ_TIMER_DD_ANSWER, g->cfg.dd_answer_ms, now_ms);
            else clear_timer(g);
        } else if (g->phase == JPQ_PHASE_FINAL_WAGER) {
            if (!g->eligible[a->actor]) { rc = reject(g, "not eligible"); break; }
            if (g->wager_set[a->actor]) { rc = reject(g, "wager already set"); break; }
            if (a->value < 0 || a->value > g->players[a->actor].score) { rc = reject(g, "wager out of range"); break; }
            g->wager[a->actor] = a->value;
            g->wager_set[a->actor] = 1;
            int all = 1;
            for (uint8_t p = 0; p < g->player_count; p++) if (!g->wager_set[p]) all = 0;
            if (all) {
                g->phase = JPQ_PHASE_FINAL_CLUE;
                if (g->cfg.final_answer_ms) start_timer(g, JPQ_TIMER_FINAL_ANSWER, g->cfg.final_answer_ms, now_ms);
                else clear_timer(g);
            }
        } else { rc = reject(g, "no wager pending"); break; }
        break;
    }
    case JPQ_ACT_JUDGE: {
        if (host_only(g, a)) { rc = -1; break; }
        int correct = a->value > 0;
        if (g->phase == JPQ_PHASE_CLUE || g->phase == JPQ_PHASE_TIEBREAK_CLUE) {
            if (a->target >= g->player_count || a->target != (uint8_t)g->buzzed) { rc = reject(g, "wrong player"); break; }
            clear_timer(g);
            g->judged[a->target] = 1; g->correct[a->target] = (uint8_t)correct;
            if (correct) {
                if (g->phase == JPQ_PHASE_TIEBREAK_CLUE) {
                    g->tiebreak_winner = (int32_t)a->target; /* marker only */
                } else {
                    add_score(g, a->target, g->act_value);
                    g->selector = a->target;
                }
                enter_reveal(g, now_ms);
            } else {
                if (g->phase != JPQ_PHASE_TIEBREAK_CLUE) add_score(g, a->target, -g->act_value);
                if (g->cfg.lockout_rebuzz) g->excluded[a->target] = 1;
                g->buzzed = (int8_t)JPQ_NOBODY;
                rebuzz_or_reveal(g, now_ms);
            }
        } else if (g->phase == JPQ_PHASE_DD_CLUE) {
            if (a->target != g->dd_selector) { rc = reject(g, "wrong player"); break; }
            clear_timer(g);
            add_score(g, a->target, correct ? g->wager[a->target] : -g->wager[a->target]);
            enter_reveal(g, now_ms);
        } else if (g->phase == JPQ_PHASE_FINAL_JUDGE) {
            if (g->judge_cursor == JPQ_NOBODY) { rc = reject(g, "judging complete"); break; }
            if (a->target != g->judge_cursor) { rc = reject(g, "judge in order"); break; }
            g->judged[a->target] = 1; g->correct[a->target] = (uint8_t)correct;
            add_score(g, a->target, correct ? g->wager[a->target] : -g->wager[a->target]);
            uint8_t next = first_eligible(g, (uint8_t)(g->judge_cursor + 1));
            g->judge_cursor = next;
            if (next == JPQ_NOBODY) { clear_timer(g); g->phase = JPQ_PHASE_RESULTS; }
        } else { rc = reject(g, "no judge pending"); break; }
        break;
    }
    case JPQ_ACT_PASS: {
        if (host_only(g, a)) { rc = -1; break; }
        if (g->phase != JPQ_PHASE_CLUE && g->phase != JPQ_PHASE_TIEBREAK_CLUE) { rc = reject(g, "no pass pending"); break; }
        if (a->target >= g->player_count || a->target != (uint8_t)g->buzzed) { rc = reject(g, "wrong player"); break; }
        g->excluded[a->target] = 1;
        g->buzzed = (int8_t)JPQ_NOBODY;
        rebuzz_or_reveal(g, now_ms);
        break;
    }
    case JPQ_ACT_REVEAL: {
        if (host_only(g, a)) { rc = -1; break; }
        if (g->phase != JPQ_PHASE_CLUE && g->phase != JPQ_PHASE_DD_CLUE && g->phase != JPQ_PHASE_TIEBREAK_CLUE) {
            rc = reject(g, "nothing to reveal"); break;
        }
        enter_reveal(g, now_ms);
        break;
    }
    case JPQ_ACT_SCORE: {
        if (host_only(g, a)) { rc = -1; break; }
        if (a->target >= g->player_count) { rc = reject(g, "unknown player"); break; }
        add_score(g, a->target, a->value);
        break;
    }
    case JPQ_ACT_ADVANCE: {
        if (host_only(g, a)) { rc = -1; break; }
        if (g->phase == JPQ_PHASE_DD_WAGER) {
            if (!g->wager_set[g->dd_selector]) { g->wager[g->dd_selector] = g->cfg.min_dd_wager; g->wager_set[g->dd_selector] = 1; }
            g->phase = JPQ_PHASE_DD_CLUE;
            if (g->cfg.dd_answer_ms) start_timer(g, JPQ_TIMER_DD_ANSWER, g->cfg.dd_answer_ms, now_ms);
            else clear_timer(g);
        } else if (g->phase == JPQ_PHASE_FINAL_WAGER) {
            for (uint8_t p = 0; p < g->player_count; p++) if (!g->wager_set[p]) { g->wager[p] = 0; g->wager_set[p] = 1; }
            g->phase = JPQ_PHASE_FINAL_CLUE;
            if (g->cfg.final_answer_ms) start_timer(g, JPQ_TIMER_FINAL_ANSWER, g->cfg.final_answer_ms, now_ms);
            else clear_timer(g);
        } else if (g->phase == JPQ_PHASE_FINAL_CLUE) {
            clear_timer(g);
            g->phase = JPQ_PHASE_FINAL_JUDGE;
            g->judge_cursor = first_eligible(g, 0);
        } else if (g->phase == JPQ_PHASE_FINAL_JUDGE) {
            uint8_t next = first_eligible(g, (uint8_t)(g->judge_cursor + 1));
            g->judge_cursor = next;
            if (next == JPQ_NOBODY) { clear_timer(g); g->phase = JPQ_PHASE_RESULTS; }
        } else if (g->phase == JPQ_PHASE_REVEAL) {
            advance_after_reveal(g, now_ms);
        } else { rc = reject(g, "nothing to advance"); break; }
        break;
    }
    case JPQ_ACT_PAUSE: {
        if (host_only(g, a)) { rc = -1; break; }
        if (g->paused) { rc = reject(g, "already paused"); break; }
        g->paused = 1;
        pause_timer(g, now_ms);
        break;
    }
    case JPQ_ACT_RESUME: {
        if (host_only(g, a)) { rc = -1; break; }
        if (!g->paused) { rc = reject(g, "not paused"); break; }
        g->paused = 0;
        resume_timer(g, now_ms);
        break;
    }
    case JPQ_ACT_RESTART: {
        if (host_only(g, a)) { rc = -1; break; }
        if (!g->restart_confirm) { g->restart_confirm = 1; break; }
        uint32_t seed = g->rng ^ 0x2545F491u;
        /* build first so a failing restart leaves scores and phase untouched */
        if (jpq_build_boards(g, seed)) { rc = reject(g, "no question pool"); break; }
        g->restart_confirm = 0;
        for (uint8_t p = 0; p < g->player_count; p++) { g->players[p].score = 0; g->players[p].ready = 0; }
        g->epoch = (g->rng ^ 0x9e3779b9u) | 1u;
        g->revision = 0;
        memset(g->last_seq, 0, sizeof g->last_seq);
        reset_clue_state(g);
        g->round = JPQ_ROUND_J;
        g->selector = 0;
        g->phase = JPQ_PHASE_LOBBY;
        g->paused = 0; g->quit = 0; g->tiebreak_active = 0;
        g->tiebreak_winner = -1;
        break;
    }
    case JPQ_ACT_QUIT: {
        if (host_only(g, a)) { rc = -1; break; }
        if (!g->quit_confirm) { g->quit_confirm = 1; break; }
        g->quit_confirm = 0;
        g->quit = 1;
        clear_timer(g);
        break;
    }
    case JPQ_ACT_TIEBREAK: {
        if (host_only(g, a)) { rc = -1; break; }
        if (g->phase != JPQ_PHASE_RESULTS) { rc = reject(g, "not at results"); break; }
        if (!g->cfg.allow_tiebreak) { rc = reject(g, "tiebreak disabled"); break; }
        if (jpq_tie_count(g) < 2) { rc = reject(g, "no tie"); break; }
        int leaders[JPQ_MAX_PLAYERS], n = 0;
        int best = g->players[jpq_leader(g)].score;
        for (uint8_t p = 0; p < g->player_count; p++)
            if (g->players[p].active && g->players[p].score == best) leaders[n++] = p;
        memset(g->eligible, 0, sizeof g->eligible);
        for (int i = 0; i < n; i++) g->eligible[leaders[i]] = 1;
        int32_t clue = -1;
        if (jpq_build_final(g, g->filter_set ? &g->filter : NULL, g->rng, &clue)) { rc = reject(g, "no reserve clue"); break; }
        g->final_clue = clue;
        g->round = JPQ_ROUND_TIEBREAK;
        g->tiebreak_active = 1;
        g->tiebreak_winner = -1;
        reset_clue_state(g);
        g->phase = JPQ_PHASE_TIEBREAK_CLUE;
        start_reading(g, now_ms);
        break;
    }
    case JPQ_ACT_RESET_HISTORY: {
        if (host_only(g, a)) { rc = -1; break; }
        jpq_reset_history(g);
        break;
    }
    case JPQ_ACT_CANCEL: {
        /* Explicitly cancel a two-step quit/restart confirmation so a stray
         * later press can never commit a confirmation the operator backed
         * out of. */
        if (host_only(g, a)) { rc = -1; break; }
        g->quit_confirm = 0;
        g->restart_confirm = 0;
        break;
    }
    default:
        rc = reject(g, "unknown action");
        break;
    }

    if (rc) return rc;
    g->last_seq[slot] = a->seq;
    g->revision++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Timer advance                                                       */
/* ------------------------------------------------------------------ */

int jpq_tick(JpqGame *g, uint64_t now_ms) {
    if (!g || g->paused) return 0;
    if (!timer_expired(g, now_ms)) return 0;
    uint8_t kind = g->timer_kind;
    clear_timer(g);
    switch (kind) {
    case JPQ_TIMER_READ:
        enter_buzz(g, now_ms, g->cfg.buzz_ms);
        break;
    case JPQ_TIMER_BUZZ:
        enter_reveal(g, now_ms);
        break;
    case JPQ_TIMER_ANSWER: {
        uint8_t p = (uint8_t)g->buzzed;
        if (p < JPQ_MAX_PLAYERS && g->players[p].active) {
            add_score(g, p, -g->act_value);
            if (g->cfg.lockout_rebuzz) g->excluded[p] = 1;
        }
        g->buzzed = (int8_t)JPQ_NOBODY;
        rebuzz_or_reveal(g, now_ms);
        break;
    }
    case JPQ_TIMER_DD_WAGER: {
        uint8_t sel = g->dd_selector;
        if (!g->wager_set[sel]) { g->wager[sel] = g->cfg.min_dd_wager; g->wager_set[sel] = 1; }
        g->phase = JPQ_PHASE_DD_CLUE;
        if (g->cfg.dd_answer_ms) start_timer(g, JPQ_TIMER_DD_ANSWER, g->cfg.dd_answer_ms, now_ms);
        break;
    }
    case JPQ_TIMER_DD_ANSWER: {
        add_score(g, g->dd_selector, -g->wager[g->dd_selector]);
        enter_reveal(g, now_ms);
        break;
    }
    case JPQ_TIMER_FINAL_WAGER: {
        for (uint8_t p = 0; p < g->player_count; p++) if (!g->wager_set[p]) { g->wager[p] = 0; g->wager_set[p] = 1; }
        g->phase = JPQ_PHASE_FINAL_CLUE;
        if (g->cfg.final_answer_ms) start_timer(g, JPQ_TIMER_FINAL_ANSWER, g->cfg.final_answer_ms, now_ms);
        break;
    }
    case JPQ_TIMER_FINAL_ANSWER:
        g->phase = JPQ_PHASE_FINAL_JUDGE;
        g->judge_cursor = first_eligible(g, 0);
        break;
    case JPQ_TIMER_TIEBREAK_ANSWER: {
        uint8_t p = (uint8_t)g->buzzed;
        if (p < JPQ_MAX_PLAYERS) g->excluded[p] = 1;
        g->buzzed = (int8_t)JPQ_NOBODY;
        rebuzz_or_reveal(g, now_ms);
        break;
    }
    default:
        return 0;
    }
    g->revision++;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Lifecycle / queries                                                 */
/* ------------------------------------------------------------------ */

int jpq_init(JpqGame *g, const JpqConfig *cfg) {
    if (!g) return -1;
    memset(g, 0, sizeof *g);
    g->cfg = cfg ? *cfg : (JpqConfig){0};
    if (!cfg) jpq_config_default(&g->cfg);
    g->epoch = 1;
    g->revision = 0;
    g->phase = JPQ_PHASE_LOBBY;
    g->buzzed = (int8_t)JPQ_NOBODY;
    g->act_col = -1; g->act_row = -1;
    g->dd_selector = JPQ_NOBODY;
    g->selector = 0;
    g->final_clue = -1;
    g->tiebreak_winner = -1;
    g->filter_set = 0;
    memset(&g->filter, 0, sizeof g->filter);
    g->filter.category = -1;
    return 0;
}

int jpq_attach(JpqGame *g, JpqDb *db, JpqHistory *hist) {
    if (!g) return -1;
    g->db = db;
    g->history = hist;
    return 0;
}

/* Store the selection filter the next START/RESTART must honor. Passing NULL
 * clears it back to the engine defaults. */
int jpq_set_filter(JpqGame *g, const JpqFilter *f) {
    if (!g) return -1;
    if (!f) { g->filter_set = 0; memset(&g->filter, 0, sizeof g->filter); g->filter.category = -1; return 0; }
    g->filter = *f;
    g->filter_set = 1;
    return 0;
}

int jpq_add_player(JpqGame *g, const char *name, uint32_t id) {
    if (!g || g->phase != JPQ_PHASE_LOBBY || g->player_count >= JPQ_MAX_PLAYERS) return -1;
    JpqPlayer *p = &g->players[g->player_count];
    memset(p, 0, sizeof *p);
    p->id = id;
    p->score = 0;
    p->active = 1;
    p->connected = 1;
    if (name) snprintf(p->name, sizeof p->name, "%s", name);
    return g->player_count++;
}

int jpq_set_ready(JpqGame *g, uint8_t idx, int ready) {
    if (!g || g->phase != JPQ_PHASE_LOBBY || idx >= g->player_count) return -1;
    g->players[idx].ready = ready ? 1 : 0;
    return 0;
}

int jpq_leader(const JpqGame *g) {
    int best = -1;
    for (uint8_t p = 0; p < g->player_count; p++)
        if (g->players[p].active && (best < 0 || g->players[p].score > g->players[best].score)) best = p;
    return best;
}

int jpq_tie_count(const JpqGame *g) {
    int lead = jpq_leader(g);
    if (lead < 0) return 0;
    int n = 0;
    for (uint8_t p = 0; p < g->player_count; p++)
        if (g->players[p].active && g->players[p].score == g->players[lead].score) n++;
    return n;
}

int jpq_can_buzz(const JpqGame *g, uint8_t p) {
    if (!g || p >= g->player_count) return 0;
    if (g->phase != JPQ_PHASE_CLUE && g->phase != JPQ_PHASE_TIEBREAK_CLUE) return 0;
    if (g->timer_kind != JPQ_TIMER_BUZZ) return 0;
    if ((uint8_t)g->buzzed != JPQ_NOBODY) return 0;
    return buzzable(g, p);
}

void jpq_wager_bounds(const JpqGame *g, uint8_t p, int32_t *min_out, int32_t *max_out, int *has_wager) {
    if (min_out) *min_out = 0;
    if (max_out) *max_out = 0;
    if (has_wager) *has_wager = 0;
    if (!g || p >= g->player_count) return;
    if (g->phase == JPQ_PHASE_DD_WAGER && p == g->dd_selector) {
        if (min_out) *min_out = g->cfg.min_dd_wager;
        if (max_out) *max_out = dd_max_wager(g, p);
        if (has_wager) *has_wager = 1;
    } else if (g->phase == JPQ_PHASE_FINAL_WAGER && g->eligible[p]) {
        if (min_out) *min_out = 0;
        if (max_out) *max_out = g->players[p].score;
        if (has_wager) *has_wager = 1;
    }
}
