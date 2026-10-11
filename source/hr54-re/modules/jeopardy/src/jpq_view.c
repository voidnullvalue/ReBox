#include "jpq_view.h"
#include "jpq_parse.h"
#ifdef HR54_RECEIVER
#include "receiver_runtime.h"
#else
#include <string.h>
#endif

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!cap) return;
    if (src) for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* Copy a JSON string node into a fixed buffer; returns 1 when the field was a
 * present string, 0 for null/missing (so the renderer can distinguish an
 * intentional redaction from an empty value). */
static int copy_node(const JpqJson *J, const JpqNode *n, char *dst, size_t cap) {
    if (!n || n->type != JPQ_JSTR) { if (cap) dst[0] = 0; return 0; }
    copy_str(dst, cap, jpq_json_str(J, n));
    return 1;
}

static int32_t num32(const JpqJson *J, int node, const char *key, int32_t dflt) {
    const JpqNode *n = jpq_json_get(J, node, key);
    return n && n->type == JPQ_JNUM ? (int32_t)n->num : dflt;
}
static uint8_t bool8(const JpqJson *J, int node, const char *key) {
    const JpqNode *n = jpq_json_get(J, node, key);
    if (!n) return 0;
    if (n->type == JPQ_JNUM) return n->num != 0 ? 1 : 0;
    return (uint8_t)(jpq_json_bool(n, 0) ? 1 : 0);
}
static unsigned unum(const JpqJson *J, int node, const char *key, unsigned dflt) {
    const JpqNode *n = jpq_json_get(J, node, key);
    return n && n->type == JPQ_JNUM ? (unsigned)n->num : dflt;
}

static void parse_player(const JpqJson *J, int node, JvPlayer *p) {
    p->id = unum(J, node, "id", 0);
    copy_node(J, jpq_json_get(J, node, "name"), p->name, sizeof p->name);
    p->score = num32(J, node, "score", 0);
    p->ready = bool8(J, node, "ready");
    p->connected = bool8(J, node, "connected");
    p->active = bool8(J, node, "active");
    p->can_buzz = bool8(J, node, "can_buzz");
    p->excluded = bool8(J, node, "excluded");
    p->eligible = bool8(J, node, "eligible");
    p->wager_set = bool8(J, node, "wager_set");
    p->answered = bool8(J, node, "answered");
    p->judged = bool8(J, node, "judged");
    p->correct = bool8(J, node, "correct");
    p->has_answer = copy_node(J, jpq_json_get(J, node, "answer"), p->answer, sizeof p->answer);
    const JpqNode *w = jpq_json_get(J, node, "wager");
    p->has_wager = w && w->type == JPQ_JNUM;
    p->wager = p->has_wager ? (int32_t)w->num : 0;
    const JpqNode *wmin = jpq_json_get(J, node, "wager_min");
    const JpqNode *wmax = jpq_json_get(J, node, "wager_max");
    p->has_wager_bounds = wmin && wmin->type == JPQ_JNUM && wmax && wmax->type == JPQ_JNUM;
    p->wager_min = p->has_wager_bounds ? (int32_t)wmin->num : 0;
    p->wager_max = p->has_wager_bounds ? (int32_t)wmax->num : 0;
}

static void parse_board(const JpqJson *J, int board, JpqView *v) {
    const JpqNode *cn = jpq_json_get(J, board, "categories");
    if (!cn) return;
    for (int c = 0; c < JPQ_VIEW_COLS; c++) {
        const JpqNode *cat = jpq_json_at(J, (int)(cn - J->nodes), (uint32_t)c);
        if (!cat) break;
        copy_node(J, jpq_json_get(J, (int)(cat - J->nodes), "name"), v->categories[c].name, sizeof v->categories[c].name);
        const JpqNode *clues = jpq_json_get(J, (int)(cat - J->nodes), "clues");
        if (!clues) continue;
        for (int r = 0; r < JPQ_VIEW_ROWS; r++) {
            const JpqNode *cell = jpq_json_at(J, (int)(clues - J->nodes), (uint32_t)r);
            if (!cell) break;
            int ci = (int)(cell - J->nodes);
            v->categories[c].value[r] = num32(J, ci, "value", 0);
            v->categories[c].used[r] = bool8(J, ci, "used");
            v->categories[c].daily_double[r] = bool8(J, ci, "daily_double");
        }
    }
}

static void parse_clue(const JpqJson *J, int node, JvClue *out) {
    out->present = 1;
    out->col = (int)unum(J, node, "col", 0);
    out->row = (int)unum(J, node, "row", 0);
    out->value = num32(J, node, "value", 0);
    out->daily_double = bool8(J, node, "daily_double");
    copy_node(J, jpq_json_get(J, node, "category"), out->category, sizeof out->category);
    out->has_text = copy_node(J, jpq_json_get(J, node, "text"), out->text, sizeof out->text);
    out->has_response = copy_node(J, jpq_json_get(J, node, "response"), out->response, sizeof out->response);
}

static void parse_final(const JpqJson *J, int node, JvFinal *out) {
    out->present = 1;
    copy_node(J, jpq_json_get(J, node, "category"), out->category, sizeof out->category);
    out->has_text = copy_node(J, jpq_json_get(J, node, "text"), out->text, sizeof out->text);
    out->has_response = copy_node(J, jpq_json_get(J, node, "response"), out->response, sizeof out->response);
}

int jpq_view_parse(JpqView *v, const char *text, size_t len) {
    JpqJson J;
    int root;
    if (!v) return -1;
    memset(v, 0, sizeof *v);
    if (jpq_json_parse(&J, text, len, &root)) return -1;
    const JpqNode *now = jpq_json_get(&J, root, "now_ms");
    v->now_ms = now && now->type == JPQ_JNUM ? (uint64_t)now->num : 0;
    v->window = unum(&J, root, "window", 0);
    const JpqNode *st = jpq_json_get(&J, root, "state");
    if (!st || st->type != JPQ_JOBJ) return -1;
    int s = (int)(st - J.nodes);
    v->epoch = unum(&J, s, "epoch", 0);
    v->revision = unum(&J, s, "revision", 0);
    v->phase = (uint8_t)unum(&J, s, "phase", 0);
    v->round = (uint8_t)unum(&J, s, "round", 0);
    v->paused = bool8(&J, s, "paused");
    /* The rules core stores the buzzing player as a signed slot (-1 = none),
     * never a boolean: keep it signed so slot 0/5 are distinguishable from
     * "nobody" and driven into the JUDGE target. */
    v->buzzed = num32(&J, s, "buzzed", -1);
    v->selector = num32(&J, s, "selector", -1);
    v->dd_selector = num32(&J, s, "dd_selector", -1);
    v->tiebreak = bool8(&J, s, "tiebreak");
    v->tiebreak_winner = num32(&J, s, "tiebreak_winner", -1);
    v->judge_cursor = (uint8_t)unum(&J, s, "judge_cursor", 0);
    v->quit = num32(&J, s, "quit", 0) ? 1 : 0;
    v->quit_confirm = bool8(&J, s, "quit_confirm");
    v->restart_confirm = bool8(&J, s, "restart_confirm");
    const JpqNode *timer = jpq_json_get(&J, s, "timer");
    if (timer) {
        int t = (int)(timer - J.nodes);
        v->timer_kind = (uint8_t)unum(&J, t, "kind", 0);
        v->timer_remaining_ms = (uint64_t)jpq_json_num(jpq_json_get(&J, t, "remaining_ms"), 0);
    }
    const JpqNode *players = jpq_json_get(&J, s, "players");
    if (players) {
        int pn = jpq_json_count(&J, (int)(players - J.nodes));
        if (pn > JPQ_VIEW_PLAYERS) pn = JPQ_VIEW_PLAYERS;
        v->player_count = pn;
        for (int i = 0; i < pn; i++) {
            const JpqNode *p = jpq_json_at(&J, (int)(players - J.nodes), (uint32_t)i);
            if (p) parse_player(&J, (int)(p - J.nodes), &v->players[i]);
        }
        /* Public podium colours (service session table, redacted-safe): the
         * wrapper carries a {playerId: name} map. Match by player id so roster
         * order changes never mis-colour a podium. */
        const JpqNode *colors = jpq_json_get(&J, root, "colors");
        if (colors && colors->type == JPQ_JOBJ) {
            int cn = (int)(colors - J.nodes);
            for (int i = 0; i < pn; i++) {
                char key[16];
                unsigned id = v->players[i].id;
                int k = 0, tmpn = 0;
                char tmp[16];
                if (!id) tmp[tmpn++] = '0';
                while (id) { tmp[tmpn++] = (char)('0' + id % 10); id /= 10; }
                while (tmpn) key[k++] = tmp[--tmpn];
                key[k] = 0;
                copy_node(&J, jpq_json_get(&J, cn, key), v->players[i].color, sizeof v->players[i].color);
            }
        }
    }
    const JpqNode *board = jpq_json_get(&J, s, "board");
    if (board) parse_board(&J, (int)(board - J.nodes), v);
    const JpqNode *cfg = jpq_json_get(&J, s, "config");
    if (cfg) {
        int c = (int)(cfg - J.nodes);
        v->cfg_read_ms = unum(&J, c, "read_ms", 0);
        v->cfg_buzz_ms = unum(&J, c, "buzz_ms", 0);
        v->cfg_answer_ms = unum(&J, c, "answer_ms", 0);
    }
    const JpqNode *clue = jpq_json_get(&J, s, "clue");
    if (clue && clue->type == JPQ_JOBJ) parse_clue(&J, (int)(clue - J.nodes), &v->clue);
    const JpqNode *fin = jpq_json_get(&J, s, "final");
    if (fin && fin->type == JPQ_JOBJ) parse_final(&J, (int)(fin - J.nodes), &v->final);
    const JpqNode *res = jpq_json_get(&J, s, "results");
    if (res) {
        int r = (int)(res - J.nodes);
        v->leader = num32(&J, r, "leader", -1);
        v->tie = num32(&J, r, "tie", 0);
    }
    v->valid = 1;
    return 0;
}

int jpq_metadata_parse(JpqMetadata *m, const char *text, size_t len) {
    JpqJson J;
    int root;
    if (!m) return -1;
    memset(m, 0, sizeof *m);
    if (jpq_json_parse(&J, text, len, &root)) return -1;
    if (J.nodes[root].type != JPQ_JOBJ) return -1;
    m->version = (int)unum(&J, root, "version", 0);
    m->protocol = (int)unum(&J, root, "protocol", 0);
    m->port = (int)unum(&J, root, "port", 0);
    copy_node(&J, jpq_json_get(&J, root, "metadataUrl"), m->url, sizeof m->url);
    copy_node(&J, jpq_json_get(&J, root, "joinPin"), m->join_pin, sizeof m->join_pin);
    copy_node(&J, jpq_json_get(&J, root, "hostPin"), m->host_pin, sizeof m->host_pin);
    copy_node(&J, jpq_json_get(&J, root, "publicDisplay"), m->display_path, sizeof m->display_path);
    copy_node(&J, jpq_json_get(&J, root, "commandSocket"), m->command_socket, sizeof m->command_socket);
    return 0;
}

uint64_t jpq_view_timer_remaining(const JpqView *view, uint64_t now_ms) {
    if (!view || view->timer_kind == JV_TIMER_NONE) return 0;
    if (now_ms <= view->now_ms) return view->timer_remaining_ms;
    uint64_t elapsed = now_ms - view->now_ms;
    return elapsed >= view->timer_remaining_ms ? 0 : view->timer_remaining_ms - elapsed;
}
