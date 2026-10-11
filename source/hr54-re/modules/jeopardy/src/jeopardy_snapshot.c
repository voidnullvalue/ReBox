/*
 * Portable display snapshot for the native uClibc renderer.
 *
 * The renderer is a dumb consumer: it renders exactly what this snapshot says.
 * viewer is a player index, JPQ_PUBLIC (the shared TV) or JPQ_HOST. JPQ_PUBLIC
 * is deliberately distinct from JPQ_HOST so the host control snapshot can never
 * be handed to a public display. Hidden clues, answers, wagers, unused
 * daily-double locations and the Final clue are redacted for everyone but the
 * host until their proper reveal phase; private Final wagers and answers are
 * exposed on the public TV sequentially during judging, one at a time, only for
 * the current or already-adjudicated player.
 */
#include "jeopardy.h"
#include "../../../reboxd/json.h"

static int is_host(uint8_t viewer) { return viewer == JPQ_HOST; }

static int may_see_wager(const JpqGame *g, uint8_t viewer, uint8_t p) {
    if (is_host(viewer) || viewer == p) return 1;
    if (g->phase == JPQ_PHASE_RESULTS) return 1;
    if (g->phase == JPQ_PHASE_FINAL_JUDGE)
        return g->eligible[p] && (p == g->judge_cursor || g->judged[p]);
    return 0;
}

static int may_see_answer(const JpqGame *g, uint8_t viewer, uint8_t p) {
    if (viewer == p) return 1;
    if (is_host(viewer)) {
        if (g->phase != JPQ_PHASE_FINAL_JUDGE) return 1;
        return p == g->judge_cursor; /* sequential: only the current answer */
    }
    if (g->phase == JPQ_PHASE_RESULTS) return 1;
    if (g->phase == JPQ_PHASE_FINAL_JUDGE)
        return g->eligible[p] && (p == g->judge_cursor || g->judged[p]);
    return 0;
}

static int may_see_response(const JpqGame *g, uint8_t viewer) {
    if (is_host(viewer)) return 1;
    /* Once Final judging begins every eligible player has already submitted
     * (that transition is what enters FINAL_JUDGE), so the public TV may show
     * the correct response as the host's adjudication reference; it can never
     * leak before, during wagering or answering. */
    return g->phase == JPQ_PHASE_REVEAL || g->phase == JPQ_PHASE_RESULTS ||
           g->phase == JPQ_PHASE_FINAL_JUDGE;
}

static void put_board(struct sb *o, const JpqGame *g, const JpqBoard *b, uint8_t viewer) {
    sb_fmt(o, "{\"round\":%u,\"categories\":[", (unsigned)b->round);
    for (int c = 0; c < JPQ_COLS; c++) {
        if (c) sb_puts(o, ",");
        const char *name = "";
        if (g->db && b->cat[c] >= 0 && (uint32_t)b->cat[c] < g->db->cat_count)
            name = g->db->cats[b->cat[c]].name;
        sb_puts(o, "{\"name\":");
        sb_json_str(o, name);
        sb_puts(o, ",\"clues\":[");
        for (int r = 0; r < JPQ_ROWS; r++) {
            if (r) sb_puts(o, ",");
            int dd = 0;
            for (uint8_t i = 0; i < b->dd_count; i++)
                if (b->dd_col[i] == c && b->dd_row[i] == r) dd = 1;
            /* unused (and even used) daily-double locations are host-only */
            int reveal_dd = dd && is_host(viewer);
            sb_fmt(o, "{\"value\":%d,\"used\":%d,\"daily_double\":%d}", b->value[c][r], b->used[c][r] ? 1 : 0, reveal_dd);
        }
        sb_puts(o, "]}");
    }
    sb_puts(o, "]}");
}

static void put_config(struct sb *o, const JpqConfig *c) {
    sb_fmt(o, "{\"read_ms\":%u,\"buzz_ms\":%u,\"answer_ms\":%u,\"dd_wager_ms\":%u,"
              "\"dd_answer_ms\":%u,\"final_wager_ms\":%u,\"final_answer_ms\":%u,"
              "\"tiebreak_answer_ms\":%u,\"min_dd_wager\":%u,\"allow_negative\":%u,"
              "\"dd_count_j\":%u,\"dd_count_dj\":%u,\"random_categories\":%u,\"allow_tiebreak\":%u}",
           c->read_ms, c->buzz_ms, c->answer_ms, c->dd_wager_ms, c->dd_answer_ms,
           c->final_wager_ms, c->final_answer_ms, c->tiebreak_answer_ms, c->min_dd_wager,
           c->allow_negative, c->dd_count_j, c->dd_count_dj, c->random_categories, c->allow_tiebreak);
}

int jpq_snapshot_json(const JpqGame *g, uint8_t viewer, char **out, size_t *outlen) {
    if (!g || !out) return -1;
    struct sb o = {0};
    sb_fmt(&o, "{\"ok\":true,\"epoch\":%u,\"revision\":%u,\"phase\":%u,\"paused\":%d,\"round\":%u,"
               "\"selector\":%d,\"buzzed\":%d,\"dd_selector\":%d,\"tiebreak\":%d,\"quit\":%d,"
               "\"quit_confirm\":%d,\"restart_confirm\":%d,\"judge_cursor\":%u,"
               "\"tiebreak_winner\":%d,",
           g->epoch, g->revision, g->phase, g->paused ? 1 : 0, g->round,
           (int)g->selector, (int)g->buzzed, (int)(int8_t)g->dd_selector,
           g->tiebreak_active ? 1 : 0, g->quit ? 1 : 0,
           g->quit_confirm ? 1 : 0, g->restart_confirm ? 1 : 0,
           g->judge_cursor, g->tiebreak_winner);
    sb_fmt(&o, "\"timer\":{\"kind\":%u,\"remaining_ms\":%llu},", g->timer_kind,
           (unsigned long long)g->timer_remaining_ms);

    sb_puts(&o, "\"players\":[");
    for (int p = 0; p < g->player_count; p++) {
        if (p) sb_puts(&o, ",");
        sb_fmt(&o, "{\"id\":%u,\"name\":", g->players[p].id);
        sb_json_str(&o, g->players[p].name);
        sb_fmt(&o, ",\"score\":%d,\"ready\":%d,\"connected\":%d,\"active\":%d,\"eligible\":%d,"
                   "\"answered\":%d,\"judged\":%d,\"correct\":%d,\"wager_set\":%d,\"excluded\":%d,"
                   "\"can_buzz\":%d",
               g->players[p].score, g->players[p].ready ? 1 : 0, g->players[p].connected ? 1 : 0,
               g->players[p].active ? 1 : 0, g->eligible[p] ? 1 : 0, g->answered[p] ? 1 : 0,
               g->judged[p] ? 1 : 0, g->correct[p] ? 1 : 0, g->wager_set[p] ? 1 : 0,
               g->excluded[p] ? 1 : 0, jpq_can_buzz(g, (uint8_t)p));
        sb_puts(&o, ",\"wager\":");
        if (may_see_wager(g, viewer, (uint8_t)p)) sb_fmt(&o, "%d", g->wager[p]);
        else sb_puts(&o, "null");
        sb_puts(&o, ",\"answer\":");
        if (may_see_answer(g, viewer, (uint8_t)p)) sb_json_str(&o, g->answer[p]);
        else sb_puts(&o, "null");
        int32_t wmin = 0, wmax = 0;
        int has = 0;
        jpq_wager_bounds(g, (uint8_t)p, &wmin, &wmax, &has);
        sb_puts(&o, ",\"wager_min\":");
        if (has) sb_fmt(&o, "%d", wmin);
        else sb_puts(&o, "null");
        sb_puts(&o, ",\"wager_max\":");
        if (has) sb_fmt(&o, "%d", wmax);
        else sb_puts(&o, "null");
        sb_puts(&o, "}");
    }
    sb_puts(&o, "],");

    sb_puts(&o, "\"config\":");
    put_config(&o, &g->cfg);
    sb_puts(&o, ",");

    const JpqBoard *b = (g->round == JPQ_ROUND_DJ) ? &g->board_dj : &g->board_j;
    sb_puts(&o, "\"board\":");
    put_board(&o, g, b, viewer);
    sb_puts(&o, ",");

    /* active clue */
    sb_puts(&o, "\"clue\":");
    if (g->act_col >= 0 && g->act_row >= 0 && g->act_col < JPQ_COLS && g->act_row < JPQ_ROWS) {
        const char *text = "", *resp = "";
        const char *cat = "";
        int32_t clue = b->clue[g->act_col][g->act_row];
        if (g->db && clue >= 0 && (uint32_t)clue < g->db->clue_count) {
            text = g->db->clues[clue].clue;
            resp = g->db->clues[clue].response;
            uint32_t ci = g->db->clues[clue].category;
            if (ci < g->db->cat_count) cat = g->db->cats[ci].name;
        }
        sb_fmt(&o, "{\"col\":%d,\"row\":%d,\"value\":%d,\"daily_double\":%d,\"category\":",
               g->act_col, g->act_row, g->act_value, g->act_is_dd ? 1 : 0);
        sb_json_str(&o, cat);
        sb_puts(&o, ",\"text\":");
        /* the daily-double clue is hidden while the selector is still wagering */
        if (g->act_is_dd && g->phase == JPQ_PHASE_DD_WAGER) sb_puts(&o, "null");
        else sb_json_str(&o, text);
        sb_puts(&o, ",\"response\":");
        if (may_see_response(g, viewer)) sb_json_str(&o, resp);
        else sb_puts(&o, "null");
        sb_puts(&o, "}");
    } else {
        sb_puts(&o, "null");
    }
    sb_puts(&o, ",");

    /* final / tiebreak clue: category at FJ, text only once wagering closed.
     * Use explicit phase membership, not ordinal comparison: JPQ_PHASE_REVEAL
     * is numerically above the Final phases, so `phase >= FINAL_CLUE` would
     * wrongly expose the Final clue during an unrelated board reveal. */
    int final_cat_visible = g->tiebreak_active ||
        (g->round == JPQ_ROUND_FINAL &&
         (g->phase == JPQ_PHASE_FINAL_WAGER || g->phase == JPQ_PHASE_FINAL_CLUE ||
          g->phase == JPQ_PHASE_FINAL_JUDGE || g->phase == JPQ_PHASE_RESULTS));
    int final_text_visible = g->tiebreak_active ||
        (g->round == JPQ_ROUND_FINAL &&
         (g->phase == JPQ_PHASE_FINAL_CLUE || g->phase == JPQ_PHASE_FINAL_JUDGE ||
          g->phase == JPQ_PHASE_RESULTS));
    sb_puts(&o, "\"final\":");
    if (final_cat_visible && g->final_clue >= 0 && g->db && (uint32_t)g->final_clue < g->db->clue_count) {
        const JpqClue *q = &g->db->clues[g->final_clue];
        const char *cat = q->category < g->db->cat_count ? g->db->cats[q->category].name : "";
        sb_puts(&o, "{\"category\":");
        sb_json_str(&o, cat);
        sb_puts(&o, ",\"text\":");
        if (final_text_visible) sb_json_str(&o, q->clue);
        else sb_puts(&o, "null");
        sb_puts(&o, ",\"response\":");
        if (may_see_response(g, viewer)) sb_json_str(&o, q->response);
        else sb_puts(&o, "null");
        sb_puts(&o, "}");
    } else {
        sb_puts(&o, "null");
    }
    sb_puts(&o, ",");

    int lead = jpq_leader(g);
    sb_fmt(&o, "\"results\":{\"leader\":%d,\"tie\":%d}}", lead, jpq_tie_count(g));

    if (o.p == NULL) return -1;
    *out = o.p;
    if (outlen) *outlen = o.len;
    return 0;
}
