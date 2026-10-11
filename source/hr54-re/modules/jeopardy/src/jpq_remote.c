#include "jpq_remote.h"

/* Small bounded string builders (no libc formatting dependency). */
static int app(char *o, size_t cap, int n, const char *s) {
    for (int i = 0; s[i] && (size_t)n + 1 < cap; i++) o[n++] = s[i];
    if ((size_t)n < cap) o[n] = 0;
    return n;
}
static int appi(char *o, size_t cap, int n, int v) {
    char t[16];
    int k = 0, neg = v < 0;
    unsigned u = neg ? (unsigned)(-(long)v) : (unsigned)v;
    if (!u) t[k++] = '0';
    while (u) { t[k++] = (char)('0' + u % 10); u /= 10; }
    if (neg && (size_t)n + 1 < cap) o[n++] = '-';
    while (k && (size_t)n + 1 < cap) o[n++] = t[--k];
    if ((size_t)n < cap) o[n] = 0;
    return n;
}

/* {"actor":"host","action":"X"} plus optional target/value/config. */
static int cmd(const char *action, int target, int value, char *out, size_t cap) {
    int n = 0;
    n = app(out, cap, n, "{\"actor\":\"host\",\"action\":\"");
    n = app(out, cap, n, action);
    n = app(out, cap, n, "\"");
    if (target >= 0) { n = app(out, cap, n, ",\"target\":"); n = appi(out, cap, n, target); }
    if (value != -1000000) { n = app(out, cap, n, ",\"value\":"); n = appi(out, cap, n, value); }
    n = app(out, cap, n, "}");
    return 1;
}

void jpq_remote_init(JpqRemote *r) {
    r->col = 0; r->row = 0; r->menu = JRM_NONE; r->cursor = 0;
    r->stop_pending = 0; r->show_host = 0; r->timer_field = 0;
    r->t_read = 0; r->t_buzz = 0; r->t_answer = 0;
    r->score_player = 0; r->score_value = 0;
}

const char *jpq_remote_menu_label(const JpqRemote *r) {
    switch (r->menu) {
    case JRM_PAUSE: return "PAUSE MENU";
    case JRM_SETTINGS: return "HOST SETTINGS";
    case JRM_CONFIRM_QUIT: return "QUIT? PRESS STOP AGAIN";
    case JRM_CONFIRM_RESTART: return "RESTART? PRESS OK";
    case JRM_HOST: return "HOST CONNECTION";
    case JRM_TIMERS: return "TIMERS  LEFT/RIGHT EDIT  OK SAVE";
    case JRM_SCORE: return "SCORE EDIT  UP/DOWN PLAYER  LEFT/RIGHT ADJUST  OK";
    default: return "";
    }
}

static int cell_used(const JpqView *v, int col, int row) {
    if (col < 0 || col >= JPQ_VIEW_COLS || row < 0 || row >= JPQ_VIEW_ROWS) return 0;
    return v->categories[col].used[row];
}

static void move_focus(JpqRemote *r, JvKey k, const JpqView *v) {
    int dc = 0, dr = 0;
    if (k == JV_KEY_LEFT) dc = -1;
    else if (k == JV_KEY_RIGHT) dc = 1;
    else if (k == JV_KEY_UP) dr = -1;
    else if (k == JV_KEY_DOWN) dr = 1;
    for (int tries = 0; tries < 30; tries++) {
        int nc = r->col + dc, nr = r->row + dr;
        if (nc < 0 || nc >= JPQ_VIEW_COLS || nr < 0 || nr >= JPQ_VIEW_ROWS) {
            if (dc) nc = (r->col + dc + JPQ_VIEW_COLS) % JPQ_VIEW_COLS;
            if (dr) nr = (r->row + dr + JPQ_VIEW_ROWS) % JPQ_VIEW_ROWS;
        }
        r->col = nc; r->row = nr;
        if (!cell_used(v, r->col, r->row)) break;
        if (!dc && !dr) break;
    }
}

static int menu_key(JpqRemote *r, JvKey k, const JpqView *v, char *out, size_t cap) {
    int items = 0;
    if (r->menu == JRM_PAUSE || r->menu == JRM_SETTINGS) items = (r->menu == JRM_PAUSE) ? 3 : 8;
    if (r->menu == JRM_CONFIRM_QUIT) {
        /* The quit/restart confirmations are two-phase in the rules core: the
         * first press armed it, the second commits. BACK/OK explicitly CANCEL
         * so a disarmed confirmation can never linger and commit later. */
        if (k == JV_KEY_STOP) { r->menu = JRM_NONE; r->stop_pending = 0; return cmd("QUIT", -1, -1000000, out, cap); }
        if (k == JV_KEY_BACK || k == JV_KEY_OK) { r->menu = JRM_NONE; r->stop_pending = 0; return cmd("CANCEL", -1, -1000000, out, cap); }
        return 0;
    }
    if (r->menu == JRM_CONFIRM_RESTART) {
        if (k == JV_KEY_OK) { r->menu = JRM_NONE; return cmd("RESTART", -1, -1000000, out, cap); }
        if (k == JV_KEY_BACK || k == JV_KEY_STOP) { r->menu = JRM_NONE; return cmd("CANCEL", -1, -1000000, out, cap); }
        return 0;
    }
    if (k == JV_KEY_BACK) { r->menu = JRM_NONE; return 0; }
    if (r->menu == JRM_HOST) {
        if (k == JV_KEY_BACK || k == JV_KEY_OK) r->menu = JRM_SETTINGS;
        return 0;
    }
    if (r->menu == JRM_SCORE) {
        int n = v->player_count ? v->player_count : 1;
        if (k == JV_KEY_UP) { r->score_player = (r->score_player + n - 1) % n; r->score_value = (int32_t)v->players[r->score_player].score; return 0; }
        if (k == JV_KEY_DOWN) { r->score_player = (r->score_player + 1) % n; r->score_value = (int32_t)v->players[r->score_player].score; return 0; }
        if (k == JV_KEY_LEFT || k == JV_KEY_RIGHT) {
            r->score_value += (k == JV_KEY_RIGHT ? 100 : -100);
            if (r->score_value < -1000000) r->score_value = -1000000;
            if (r->score_value > 1000000) r->score_value = 1000000;
            return 0;
        }
        if (k == JV_KEY_BACK) { r->menu = JRM_SETTINGS; return 0; }
        if (k == JV_KEY_OK && v->player_count) {
            r->menu = JRM_SETTINGS;
            return cmd("SCORE", (int)v->players[r->score_player].id, (int)r->score_value, out, cap);
        }
        return 0;
    }
    if (r->menu == JRM_TIMERS) {
        if (k == JV_KEY_UP) { r->timer_field = (r->timer_field + 2) % 3; return 0; }
        if (k == JV_KEY_DOWN) { r->timer_field = (r->timer_field + 1) % 3; return 0; }
        if (k == JV_KEY_LEFT || k == JV_KEY_RIGHT) {
            int d = (k == JV_KEY_RIGHT ? 1 : -1) * 1000;
            int32_t *f = r->timer_field == 0 ? &r->t_read : r->timer_field == 1 ? &r->t_buzz : &r->t_answer;
            *f += d;
            if (*f < 1000) *f = 1000;
            if (*f > 60000) *f = 60000;
            return 0;
        }
        if (k == JV_KEY_BACK) { r->menu = JRM_SETTINGS; return 0; }
        if (k == JV_KEY_OK) {
            int n = 0;
            n = app(out, cap, n, "{\"actor\":\"host\",\"action\":\"CONFIG\",\"config\":{\"read_ms\":");
            n = appi(out, cap, n, r->t_read);
            n = app(out, cap, n, ",\"buzz_ms\":");
            n = appi(out, cap, n, r->t_buzz);
            n = app(out, cap, n, ",\"answer_ms\":");
            n = appi(out, cap, n, r->t_answer);
            n = app(out, cap, n, "}}");
            r->menu = JRM_SETTINGS;
            return 1;
        }
        return 0;
    }
    if (k == JV_KEY_UP && items) { r->cursor = (r->cursor + items - 1) % items; return 0; }
    if (k == JV_KEY_DOWN && items) { r->cursor = (r->cursor + 1) % items; return 0; }
    if (k != JV_KEY_OK) return 0;
    if (r->menu == JRM_PAUSE) {
        if (r->cursor == 0) { r->menu = JRM_NONE; return cmd("RESUME", -1, -1000000, out, cap); }
        if (r->cursor == 1) { r->menu = JRM_CONFIRM_RESTART; return cmd("RESTART", -1, -1000000, out, cap); }
        r->menu = JRM_CONFIRM_QUIT; return cmd("QUIT", -1, -1000000, out, cap);
    }
    /* settings */
    if (r->cursor == 0) { r->menu = JRM_NONE; return cmd("RESUME", -1, -1000000, out, cap); }
    if (r->cursor == 1) { r->menu = JRM_CONFIRM_RESTART; return cmd("RESTART", -1, -1000000, out, cap); }
    if (r->cursor == 2) { r->menu = JRM_CONFIRM_QUIT; return cmd("QUIT", -1, -1000000, out, cap); }
    if (r->cursor == 3) { r->score_player = 0; r->score_value = v->player_count ? (int32_t)v->players[0].score : 0; r->menu = JRM_SCORE; return 0; }
    if (r->cursor == 4) { r->menu = JRM_HOST; r->show_host = 1; return 0; }
    if (r->cursor == 5) {
        r->t_read = (int32_t)v->cfg_read_ms;
        r->t_buzz = (int32_t)v->cfg_buzz_ms;
        r->t_answer = (int32_t)v->cfg_answer_ms;
        r->timer_field = 0;
        r->menu = JRM_TIMERS; return 0;
    }
    if (r->cursor == 6) { r->menu = JRM_NONE; return cmd("REVEAL", -1, -1000000, out, cap); }
    if (r->cursor == 7) {
        int b = (v->buzzed >= 0 && v->buzzed < v->player_count) ? v->buzzed : -1;
        r->menu = JRM_NONE;
        if (b >= 0) return cmd("PASS", (int)v->players[b].id, -1000000, out, cap);
        return 0;
    }
    return 0;
}

int jpq_remote_key(JpqRemote *r, JvKey k, const JpqView *v, char *out, size_t cap) {
    if (!r || !out || cap < 8) return -1;
    out[0] = 0;
    if (k == JV_KEY_NONE) return 0;
    if (r->menu != JRM_NONE) return menu_key(r, k, v, out, cap);

    if (k == JV_KEY_STOP) {
        /* First STOP arms the rules quit (two-phase); the second commits. */
        r->stop_pending = 1;
        r->menu = JRM_CONFIRM_QUIT;
        return cmd("QUIT", -1, -1000000, out, cap);
    }
    if (k == JV_KEY_INFO) { r->menu = JRM_SETTINGS; r->cursor = 0; return 0; }
    /* While the game is paused (the pause overlay is showing) OK/BACK are the
     * explicit resume, so the operator is never stranded on a frozen board. */
    if (v->paused) {
        if (k == JV_KEY_OK || k == JV_KEY_BACK) return cmd("RESUME", -1, -1000000, out, cap);
        return 0;
    }
    if (k == JV_KEY_BACK) {
        if (v->phase == JV_LOBBY) return 0;
        r->menu = JRM_PAUSE; r->cursor = 0;
        return cmd("PAUSE", -1, -1000000, out, cap);
    }

    switch (v->phase) {
    case JV_LOBBY:
        if (k == JV_KEY_OK) return cmd("START", -1, -1000000, out, cap);
        return 0;
    case JV_BOARD:
        if (k == JV_KEY_UP || k == JV_KEY_DOWN || k == JV_KEY_LEFT || k == JV_KEY_RIGHT) {
            move_focus(r, k, v);
            return 0;
        }
        if (k == JV_KEY_OK) {
            if (cell_used(v, r->col, r->row)) return 0;
            return cmd("SELECT", r->col * JPQ_VIEW_ROWS + r->row, -1000000, out, cap);
        }
        return 0;
    case JV_FINAL_JUDGE:
        if (k == JV_KEY_OK && v->judge_cursor < v->player_count)
            return cmd("JUDGE", (int)v->players[v->judge_cursor].id, 1, out, cap);
        if (k == JV_KEY_RED && v->judge_cursor < v->player_count)
            return cmd("JUDGE", (int)v->players[v->judge_cursor].id, -1, out, cap);
        if (k == JV_KEY_INFO) { r->menu = JRM_SETTINGS; return 0; }
        return 0;
    case JV_CLUE: case JV_TIEBREAK_CLUE:
        /* Judge the player who actually buzzed (a signed slot), never the
         * board selector: OK = correct, RED = incorrect. With no live buzzer
         * the reveal/pass live in the INFO host menu, not on OK. */
        if (v->buzzed >= 0 && v->buzzed < v->player_count) {
            int id = (int)v->players[v->buzzed].id;
            if (k == JV_KEY_OK) return cmd("JUDGE", id, 1, out, cap);
            if (k == JV_KEY_RED) return cmd("JUDGE", id, -1, out, cap);
        }
        return 0;
    case JV_DD_WAGER:
        /* No answer yet: OK accepts the floor wager and advances. */
        if (k == JV_KEY_OK) return cmd("ADVANCE", -1, -1000000, out, cap);
        return 0;
    case JV_DD_CLUE: {
        int sel = (v->dd_selector >= 0 && v->dd_selector < v->player_count) ? v->dd_selector : -1;
        if (sel >= 0) {
            int id = (int)v->players[sel].id;
            if (k == JV_KEY_OK) return cmd("JUDGE", id, 1, out, cap);
            if (k == JV_KEY_RED) return cmd("JUDGE", id, -1, out, cap);
        }
        return 0;
    }
    case JV_REVEAL: case JV_RESULTS:
        if (k == JV_KEY_OK) return cmd("ADVANCE", -1, -1000000, out, cap);
        return 0;
    default:
        if (k == JV_KEY_OK) return cmd("ADVANCE", -1, -1000000, out, cap);
        return 0;
    }
}
