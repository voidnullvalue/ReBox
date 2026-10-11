#define _GNU_SOURCE
/*
 * Authoritative Jeopardy network service. See jeopardy_service.h.
 *
 * All state lives in one thread (the network poll loop). Every accepted
 * mutation is persisted and broadcast; timers persist only their remaining
 * duration and never force a disk write on each countdown tick. Reconnect
 * tokens are 256-bit values stored 0600; PINs live only in the private native
 * metadata directory and are never served on the LAN.
 */
#include "jeopardy_service.h"
#include "../../../reboxd/core.h"

#if defined(__has_include)
#  if __has_include("web_assets.h")
#    include "web_assets.h"
#  endif
#endif
#ifndef JPQ_WEB_ASSET_INCLUDED
static const char JPQ_WEB_HTML[] = "<!doctype html><html><body><h1>Jeopardy service</h1></body></html>";
#define JPQ_WEB_HTML_LEN (unsigned)(sizeof(JPQ_WEB_HTML) - 1u)
#endif

#define JPQ_MAX_VIEWERS 24
#define JPQ_TOKEN_HEX 64
#define JPQ_SVC_CONFIG (-2)
#define JPQ_SVC_FILTER (-3)
#define JPQ_SVC_REMOVE (-4)
#define JPQ_SVC_RESET_LOBBY (-5)
#define JPQ_JOIN_GLOBAL_MAX 60
#define JPQ_JOIN_WINDOW_MS 10000u

typedef struct {
    int used;
    uint32_t id;
    char token[JPQ_TOKEN_HEX + 1];
    char color[16];
} Session;

typedef struct {
    int used;
    JpqConnId conn;
    int role;
    uint32_t player_id;
    JpqConnId hello_sent;
} Viewer;

struct JpqService {
    JpqDb db;
    JpqHistory hist;
    JpqGame game;
    char data_dir[1024];
    char db_path[1024];
    char command_sock[1200];
    char public_url[256];
    int port;
    uint32_t next_player_id;
    uint32_t window;
    int32_t prev_prompt;
    Session sessions[JPQ_MAX_PLAYERS];
    char host_token[JPQ_TOKEN_HEX + 1];
    char join_pin[8];
    char host_pin[10];
    Viewer viewers[JPQ_MAX_VIEWERS];
    JpqSendFn send;
    JpqCloseFn closef;
    void *netud;
    int dirty_save, dirty_broadcast;
    int save_failed;           /* latched: a durable write failed */
    int pause_request;         /* atomic flag set by the native lifecycle */
    int relaunch_request;      /* atomic: poll thread must reset quit/confirm */
    int relaunch_ack;          /* atomic: reset applied and display rewritten */
    int native_gate;           /* atomic: installed LAN run requires native */
    int native_active;         /* atomic: native surface currently running */
    int quit;
    uint64_t join_window;
    int join_attempts;
};

static const char *const JPQ_PALETTE[6] = {"cobalt", "gold", "crimson", "jade", "violet", "amber"};

static uint64_t ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static uint32_t clamp_u32(double v, uint32_t lo, uint32_t hi) {
    if (!(v >= (double)lo)) return lo;
    if (v > (double)hi) return hi;
    return (uint32_t)v;
}

static int64_t ival(struct jval *o, const char *k, int64_t def) {
    struct jval *v = jget(o, k);
    return v ? (int64_t)jnum(v, (double)def) : def;
}

static const char *sval(struct jval *o, const char *k) {
    return jstr(jget(o, k));
}

/* ------------------------------------------------------------------ */
/* Random credentials                                                   */
/* ------------------------------------------------------------------ */

static int fill_random(unsigned char *b, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, b + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { close(fd); return -1; }
        got += (size_t)r;
    }
    close(fd);
    return 0;
}

static int make_token(char out[JPQ_TOKEN_HEX + 1]) {
    unsigned char b[32];
    if (fill_random(b, sizeof b)) return -1;
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[i * 2] = hexd[b[i] >> 4]; out[i * 2 + 1] = hexd[b[i] & 15]; }
    out[JPQ_TOKEN_HEX] = 0;
    return 0;
}

/* Unbiased PIN digits: reject random bytes >= 250 so every digit 0-9 is equally
 * likely. Returns -1 (issuing no usable PIN) if the entropy source fails, so
 * startup can fail closed instead of falling back to a predictable all-zero
 * PIN. */
static int make_pin(char *out, int digits) {
    unsigned char b[32];
    int got = 0;
    while (got < digits) {
        if (fill_random(b, sizeof b)) return -1;
        for (int i = 0; i < (int)sizeof b && got < digits; i++)
            if (b[i] < 250) out[got++] = (char)('0' + (b[i] % 10));
    }
    out[digits] = 0;
    return 0;
}

/* Constant-time string equality: the loop count depends only on the longer
 * input (never on where the bytes differ) and a length mismatch is folded into
 * the accumulated result, so a wrong token cannot be probed by timing. */
static int tok_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    size_t na = strlen(a), nb = strlen(b);
    size_t n = na > nb ? na : nb;
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = i < na ? (unsigned char)a[i] : 0;
        unsigned char cb = i < nb ? (unsigned char)b[i] : 0;
        d |= (unsigned char)(ca ^ cb);
    }
    return d == 0;
}

/* Fixed-length compare for the short 4/6 digit PINs: the loop always runs 16
 * iterations so the supplied length leaks nothing about the secret. */
static int pin_eq(const char *given, const char *expected) {
    size_t en = expected ? strlen(expected) : 0, gn = given ? strlen(given) : 0;
    unsigned char d = 0;
    for (size_t i = 0; i < 16; i++) {
        unsigned char e = i < en ? (unsigned char)expected[i] : 0;
        unsigned char g = i < gn ? (unsigned char)given[i] : 0;
        d |= (unsigned char)(e ^ g);
    }
    if (gn >= 16) d |= 1;
    return d == 0;
}

/* ------------------------------------------------------------------ */
/* Lookups                                                              */
/* ------------------------------------------------------------------ */

static int slot_by_id(const JpqService *s, uint32_t id) {
    for (int i = 0; i < JPQ_MAX_PLAYERS; i++)
        if (i < s->game.player_count && s->game.players[i].id == id) return i;
    return -1;
}

static int session_by_id(const JpqService *s, uint32_t id) {
    for (int i = 0; i < JPQ_MAX_PLAYERS; i++)
        if (s->sessions[i].used && s->sessions[i].id == id) return i;
    return -1;
}

static int session_by_token(const JpqService *s, const char *token) {
    if (!token || !*token) return -1;
    for (int i = 0; i < JPQ_MAX_PLAYERS; i++)
        if (s->sessions[i].used && tok_eq(s->sessions[i].token, token)) return i;
    return -1;
}

static Viewer *viewer_by_conn(JpqService *s, JpqConnId conn) {
    for (int i = 0; i < JPQ_MAX_VIEWERS; i++)
        if (s->viewers[i].used && s->viewers[i].conn == conn) return &s->viewers[i];
    return NULL;
}

static Viewer *viewer_by_player(JpqService *s, uint32_t id) {
    for (int i = 0; i < JPQ_MAX_VIEWERS; i++)
        if (s->viewers[i].used && s->viewers[i].role == JPQ_ROLE_PLAYER && s->viewers[i].player_id == id)
            return &s->viewers[i];
    return NULL;
}

static const char *color_for_id(const JpqService *s, uint32_t id) {
    int si = session_by_id(s, id);
    return si >= 0 ? s->sessions[si].color : NULL;
}

static int color_taken(const JpqService *s, const char *color, uint32_t except) {
    for (int i = 0; i < JPQ_MAX_PLAYERS; i++)
        if (i < s->game.player_count && s->game.players[i].id != except &&
            color_for_id(s, s->game.players[i].id) && !strcmp(color_for_id(s, s->game.players[i].id), color))
            return 1;
    return 0;
}

static int name_taken(const JpqService *s, const char *name, uint32_t except) {
    for (int i = 0; i < JPQ_MAX_PLAYERS; i++)
        if (i < s->game.player_count && s->game.players[i].id != except &&
            !strcasecmp(s->game.players[i].name, name))
            return 1;
    return 0;
}

static int svc_path(const JpqService *s, char *out, size_t cap, const char *suffix) {
    int n = snprintf(out, cap, "%s/%s", s->data_dir, suffix);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Persistence                                                          */
/* ------------------------------------------------------------------ */

static int save_sessions(const JpqService *s) {
    struct sb o = {0};
    sb_fmt(&o, "{\"version\":1,\"nextPlayerId\":%u,\"hostToken\":", s->next_player_id);
    sb_json_str(&o, s->host_token);
    sb_puts(&o, ",\"players\":[");
    int first = 1;
    for (int i = 0; i < JPQ_MAX_PLAYERS; i++) {
        if (!s->sessions[i].used) continue;
        if (!first) sb_puts(&o, ",");
        first = 0;
        sb_fmt(&o, "{\"id\":%u,\"token\":", s->sessions[i].id);
        sb_json_str(&o, s->sessions[i].token);
        sb_puts(&o, ",\"color\":");
        sb_json_str(&o, s->sessions[i].color);
        sb_puts(&o, "}");
    }
    sb_puts(&o, "]}");
    char path[1200];
    int rc = -1;
    if (o.p && !svc_path(s, path, sizeof path, "jeopardy-sessions.json")) rc = rb_atomic(path, o.p, o.len);
    free(o.p);
    return rc;
}

static void load_sessions(JpqService *s) {
    char path[1200];
    if (svc_path(s, path, sizeof path, "jeopardy-sessions.json")) return;
    size_t len = 0;
    char *raw = rb_read(path, 256 * 1024, &len);
    if (!raw) return;
    struct jval *v = json_parse(raw, len);
    if (v && v->t == J_OBJ) {
        s->next_player_id = (uint32_t)jnum(jget(v, "nextPlayerId"), 0);
        const char *ht = jstr(jget(v, "hostToken"));
        if (ht && strlen(ht) == JPQ_TOKEN_HEX) snprintf(s->host_token, sizeof s->host_token, "%s", ht);
        struct jval *players = jget(v, "players");
        if (players && players->t == J_ARR) {
            for (size_t i = 0; i < players->n && i < JPQ_MAX_PLAYERS; i++) {
                struct jval *p = jnth(players, i);
                const char *tok = jstr(jget(p, "token"));
                const char *col = jstr(jget(p, "color"));
                if (!tok || strlen(tok) != JPQ_TOKEN_HEX) continue;
                Session *se = &s->sessions[i];
                se->used = 1;
                se->id = (uint32_t)jnum(jget(p, "id"), 0);
                snprintf(se->token, sizeof se->token, "%s", tok);
                snprintf(se->color, sizeof se->color, "%s", col ? col : JPQ_PALETTE[i % 6]);
                if (se->id >= s->next_player_id) s->next_player_id = se->id + 1;
            }
        }
    }
    jfree(v);
    free(raw);
}

/* Persist the game, clue history and sessions. Returns 0 only when every
 * durable write succeeded. On any failure the error is latched and the dirty
 * flag is kept set, so a transient disk problem is retried rather than being
 * silently reported as saved. */
static int service_save(JpqService *s, uint64_t now) {
    JpqGame tmp = s->game;
    if (tmp.timer_deadline_ms && !tmp.paused) {
        tmp.timer_remaining_ms = tmp.timer_deadline_ms > now ? tmp.timer_deadline_ms - now : 0;
    }
    int rc = jpq_save(&tmp, s->data_dir);
    char path[1200];
    if (s->hist.bits) {
        if (svc_path(s, path, sizeof path, "jeopardy-history.bin")) rc = -1;
        else if (jpq_history_save(&s->hist, path)) rc = -1;
    }
    if (save_sessions(s)) rc = -1;
    if (rc) { s->save_failed = 1; return -1; }
    s->dirty_save = 0;
    s->save_failed = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Native display + metadata                                            */
/* ------------------------------------------------------------------ */

static int native_dir(JpqService *s, char *out, size_t cap) {
    if (svc_path(s, out, cap, "native")) return -1;
    return rb_mkdir(out);
}

static int write_metadata(JpqService *s) {
    char dir[1200], path[1300], display[1300];
    if (native_dir(s, dir, sizeof dir) || snprintf(path, sizeof path, "%s/metadata.json", dir) >= (int)sizeof path ||
        snprintf(display, sizeof display, "%s/public-display.json", dir) >= (int)sizeof display)
        return -1;
    struct sb o = {0};
    sb_puts(&o, "{\"version\":1,\"service\":\"jeopardy\",\"protocol\":1,\"metadataUrl\":");
    sb_json_str(&o, s->public_url);
    sb_puts(&o, ",\"joinPin\":");
    sb_json_str(&o, s->join_pin);
    sb_puts(&o, ",\"hostPin\":");
    sb_json_str(&o, s->host_pin);
    sb_puts(&o, ",\"publicDisplay\":");
    sb_json_str(&o, display);
    sb_puts(&o, ",\"commandSocket\":");
    sb_json_str(&o, s->command_sock);
    sb_fmt(&o, ",\"port\":%d}", s->port);
    int rc = o.p ? rb_atomic(path, o.p, o.len) : -1;
    free(o.p);
    return rc;
}

static void write_public_display(JpqService *s, uint64_t now) {
    char dir[1200], path[1300];
    if (native_dir(s, dir, sizeof dir) || snprintf(path, sizeof path, "%s/public-display.json", dir) >= (int)sizeof path)
        return;
    JpqGame tmp = s->game;
    if (tmp.timer_deadline_ms && !tmp.paused)
        tmp.timer_remaining_ms = tmp.timer_deadline_ms > now ? tmp.timer_deadline_ms - now : 0;
    char *snap = NULL;
    size_t sn = 0;
    if (jpq_snapshot_json(&tmp, JPQ_PUBLIC, &snap, &sn)) return;
    struct sb o = {0};
    /* now_ms is the receiver monotonic clock at snapshot time, so the renderer
     * can derive the live deadline (now_ms + timer.remaining_ms) and count down
     * locally without the service rewriting this file on every tick. The public
     * podium colours ride alongside the state (never inside it) so the core
     * snapshot stays exactly the same for every viewer. */
    sb_fmt(&o, "{\"window\":%u,\"now_ms\":%llu,\"colors\":{", s->window, (unsigned long long)now);
    int first_color = 1;
    for (int i = 0; i < tmp.player_count; i++) {
        if (!tmp.players[i].active) continue;
        if (!first_color) sb_puts(&o, ",");
        first_color = 0;
        sb_fmt(&o, "\"%u\":", tmp.players[i].id);
        const char *col = color_for_id(s, tmp.players[i].id);
        sb_json_str(&o, col ? col : "");
    }
    sb_puts(&o, "},\"state\":");
    sb_putn(&o, snap, sn);
    sb_puts(&o, "}");
    free(snap);
    if (o.p) rb_atomic(path, o.p, o.len);
    free(o.p);
}

/* ------------------------------------------------------------------ */
/* Window (clue generation / open-window id)                            */
/* ------------------------------------------------------------------ */

static int32_t prompt_token(const JpqService *s) {
    const JpqGame *g = &s->game;
    switch (g->phase) {
    case JPQ_PHASE_CLUE:
    case JPQ_PHASE_TIEBREAK_CLUE:
        return 100000 + g->act_col * 1000 + g->act_row * 10 + (g->timer_kind == JPQ_TIMER_BUZZ ? 1 : 2);
    case JPQ_PHASE_DD_WAGER: return 200000 + g->act_col * 1000 + g->act_row * 10;
    case JPQ_PHASE_DD_CLUE: return 210000 + g->act_col * 1000 + g->act_row * 10;
    case JPQ_PHASE_FINAL_WAGER: return 300000;
    case JPQ_PHASE_FINAL_CLUE: return 301000;
    case JPQ_PHASE_FINAL_JUDGE: return 302000 + g->judge_cursor;
    default: return 0;
    }
}

static void update_window(JpqService *s) {
    int32_t p = prompt_token(s);
    if (p == 0) { s->prev_prompt = 0; return; }
    if (p != s->prev_prompt) { s->prev_prompt = p; s->window++; }
}

/* ------------------------------------------------------------------ */
/* Snapshots + broadcast                                                */
/* ------------------------------------------------------------------ */

static unsigned seq_for(const JpqService *s, int role, uint32_t player_id) {
    if (role == JPQ_ROLE_HOST) return s->game.last_seq[6];
    int slot = slot_by_id(s, player_id);
    if (slot >= 0) return s->game.last_seq[slot];
    return 0;
}

static char *state_message(JpqService *s, int role, uint32_t player_id, uint64_t now, size_t *outlen) {
    uint8_t viewer = role == JPQ_ROLE_HOST ? JPQ_HOST : (role == JPQ_ROLE_PLAYER ? (uint8_t)slot_by_id(s, player_id) : JPQ_PUBLIC);
    if (role == JPQ_ROLE_PLAYER && (int8_t)viewer < 0) return NULL;
    JpqGame tmp = s->game;
    if (tmp.timer_deadline_ms && !tmp.paused)
        tmp.timer_remaining_ms = tmp.timer_deadline_ms > now ? tmp.timer_deadline_ms - now : 0;
    char *snap = NULL;
    size_t sn = 0;
    if (jpq_snapshot_json(&tmp, viewer, &snap, &sn)) return NULL;
    struct sb o = {0};
    sb_fmt(&o, "{\"type\":\"state\",\"epoch\":%u,\"revision\":%u,\"window\":%u,\"seq\":%u,\"role\":",
           tmp.epoch, tmp.revision, s->window, seq_for(s, role, player_id));
    sb_json_str(&o, role == JPQ_ROLE_HOST ? "host" : role == JPQ_ROLE_PLAYER ? "player" : "public");
    sb_puts(&o, ",\"you\":");
    if (role == JPQ_ROLE_PLAYER) {
        sb_fmt(&o, "{\"playerId\":%u,\"color\":", player_id);
        sb_json_str(&o, color_for_id(s, player_id) ? color_for_id(s, player_id) : "");
        sb_puts(&o, "}");
    } else {
        sb_puts(&o, "null");
    }
    sb_puts(&o, ",\"colors\":{");
    int first = 1;
    for (int i = 0; i < tmp.player_count; i++) {
        if (!tmp.players[i].active) continue;
        if (!first) sb_puts(&o, ",");
        first = 0;
        sb_fmt(&o, "\"%u\":", tmp.players[i].id);
        const char *col = color_for_id(s, tmp.players[i].id);
        sb_json_str(&o, col ? col : "");
    }
    sb_puts(&o, "},\"state\":");
    sb_putn(&o, snap, sn);
    sb_puts(&o, "}");
    free(snap);
    if (outlen) *outlen = o.len;
    return o.p;
}

static void send_to(JpqService *s, JpqConnId conn, const char *data, size_t len) {
    if (s->send) s->send(s->netud, conn, data, len);
}

static void send_error(JpqService *s, JpqConnId conn, const char *code, const char *msg) {
    struct sb o = {0};
    sb_puts(&o, "{\"type\":\"error\",\"code\":");
    sb_json_str(&o, code);
    sb_puts(&o, ",\"error\":");
    sb_json_str(&o, msg);
    sb_puts(&o, "}");
    if (o.p) send_to(s, conn, o.p, o.len);
    free(o.p);
}

static void send_state(JpqService *s, Viewer *v, uint64_t now) {
    if (!v->used || v->role == JPQ_ROLE_UNAUTH) return;
    size_t len = 0;
    char *msg = state_message(s, v->role, v->player_id, now, &len);
    if (msg) { send_to(s, v->conn, msg, len); free(msg); }
}

static void broadcast(JpqService *s, uint64_t now) {
    for (int i = 0; i < JPQ_MAX_VIEWERS; i++) {
        Viewer *v = &s->viewers[i];
        if (!v->used || v->role == JPQ_ROLE_UNAUTH) continue;
        send_state(s, v, now);
    }
    write_public_display(s, now);
    s->dirty_broadcast = 0;
}

static void service_flush(JpqService *s, uint64_t now) {
    if (s->dirty_save) service_save(s, now);
    if (s->dirty_broadcast) broadcast(s, now);
}

/* Reconcile expired timers on the receiver clock before any action is applied,
 * so a late DD/Final wager or answer (or an overdue judge) cannot be accepted
 * against a window that expired but had not yet been polled. */
static void service_pretick(JpqService *s, uint64_t now) {
    if (jpq_tick(&s->game, now)) {
        update_window(s);
        s->dirty_save = s->dirty_broadcast = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Action handling                                                      */
/* ------------------------------------------------------------------ */

static int is_service_action(const char *n) {
    return !strcmp(n, "CONFIG") || !strcmp(n, "FILTER") || !strcmp(n, "REMOVE") || !strcmp(n, "RESET_LOBBY");
}

static int action_code(const char *n) {
    if (!strcmp(n, "READY")) return JPQ_ACT_READY;
    if (!strcmp(n, "START")) return JPQ_ACT_START;
    if (!strcmp(n, "SELECT")) return JPQ_ACT_SELECT_CLUE;
    if (!strcmp(n, "BUZZ")) return JPQ_ACT_BUZZ;
    if (!strcmp(n, "SUBMIT")) return JPQ_ACT_SUBMIT;
    if (!strcmp(n, "WAGER")) return JPQ_ACT_WAGER;
    if (!strcmp(n, "JUDGE")) return JPQ_ACT_JUDGE;
    if (!strcmp(n, "PASS")) return JPQ_ACT_PASS;
    if (!strcmp(n, "REVEAL")) return JPQ_ACT_REVEAL;
    if (!strcmp(n, "SCORE")) return JPQ_ACT_SCORE;
    if (!strcmp(n, "ADVANCE")) return JPQ_ACT_ADVANCE;
    if (!strcmp(n, "PAUSE")) return JPQ_ACT_PAUSE;
    if (!strcmp(n, "RESUME")) return JPQ_ACT_RESUME;
    if (!strcmp(n, "RESTART")) return JPQ_ACT_RESTART;
    if (!strcmp(n, "QUIT")) return JPQ_ACT_QUIT;
    if (!strcmp(n, "TIEBREAK")) return JPQ_ACT_TIEBREAK;
    if (!strcmp(n, "RESET_HISTORY")) return JPQ_ACT_RESET_HISTORY;
    if (!strcmp(n, "CANCEL")) return JPQ_ACT_CANCEL;
    return -1;
}

static int do_config(JpqService *s, struct jval *m) {
    JpqConfig *c = &s->game.cfg;
    struct jval *o = jget(m, "config");
    if (!o || o->t != J_OBJ) return -1;
#define SET_U32(field, lo, hi) if (jget(o, #field)) c->field = (uint32_t)clamp_u32(jnum(jget(o, #field), (double)c->field), lo, hi)
#define SET_U8(field, lo, hi) if (jget(o, #field)) c->field = (uint8_t)clamp_u32(jnum(jget(o, #field), (double)c->field), lo, hi)
    SET_U32(read_ms, 0, 600000); SET_U32(buzz_ms, 0, 600000); SET_U32(answer_ms, 0, 600000);
    SET_U32(dd_wager_ms, 0, 600000); SET_U32(dd_answer_ms, 0, 600000);
    SET_U32(final_wager_ms, 0, 600000); SET_U32(final_answer_ms, 0, 600000);
    SET_U32(tiebreak_answer_ms, 0, 600000);
    SET_U8(allow_negative, 0, 1); SET_U8(lockout_rebuzz, 0, 1); SET_U8(random_categories, 0, 1);
    SET_U8(allow_tiebreak, 0, 1); SET_U8(dd_count_j, 0, 2); SET_U8(dd_count_dj, 0, 2);
    SET_U8(min_dd_wager, 0, 200);
#undef SET_U32
#undef SET_U8
    return 0;
}

static int do_filter(JpqService *s, struct jval *m) {
    struct jval *o = jget(m, "filter");
    if (!o || o->t != J_OBJ) { jpq_set_filter(&s->game, NULL); return 0; }
    JpqFilter f;
    memset(&f, 0, sizeof f);
    f.year_min = (uint32_t)ival(o, "year_min", 0);
    f.year_max = (uint32_t)ival(o, "year_max", 0);
    f.difficulty_min = (int)ival(o, "difficulty_min", 0);
    f.difficulty_max = (int)ival(o, "difficulty_max", 0);
    f.category = jget(o, "category") ? (int)ival(o, "category", -1) : -1;
    f.episode = (uint32_t)ival(o, "episode", 0);
    jpq_set_filter(&s->game, &f);
    return 0;
}

static int do_remove(JpqService *s, uint32_t target_id, char *err, size_t errsz) {
    int slot = slot_by_id(s, target_id);
    if (slot < 0) { snprintf(err, errsz, "unknown player"); return -1; }
    int si = session_by_id(s, target_id);
    if (si >= 0) { memset(&s->sessions[si], 0, sizeof s->sessions[si]); }
    Viewer *v = viewer_by_player(s, target_id);
    if (v && s->closef) { s->closef(s->netud, v->conn, 1000); v->used = 0; }
    if (s->game.phase == JPQ_PHASE_LOBBY) {
        for (int i = slot; i + 1 < s->game.player_count; i++) s->game.players[i] = s->game.players[i + 1];
        s->game.player_count--;
        memset(&s->game.players[s->game.player_count], 0, sizeof s->game.players[0]);
    } else {
        s->game.players[slot].active = 0;
        s->game.players[slot].connected = 0;
        if (s->game.selector == slot) {
            for (int i = 0; i < s->game.player_count; i++)
                if (s->game.players[i].active) { s->game.selector = (uint8_t)i; break; }
        }
    }
    return 0;
}

static int do_reset_lobby(JpqService *s) {
    s->game.phase = JPQ_PHASE_LOBBY;
    s->game.paused = 0;
    s->game.timer_kind = JPQ_TIMER_NONE;
    s->game.timer_deadline_ms = 0;
    s->game.timer_remaining_ms = 0;
    s->game.act_col = -1;
    s->game.act_row = -1;
    s->game.buzzed = (int8_t)JPQ_NOBODY;
    s->game.tiebreak_active = 0;
    s->game.quit = 0;
    s->game.quit_confirm = 0;
    s->game.restart_confirm = 0;
    memset(s->game.excluded, 0, sizeof s->game.excluded);
    memset(s->game.answered, 0, sizeof s->game.answered);
    memset(s->game.judged, 0, sizeof s->game.judged);
    memset(s->game.correct, 0, sizeof s->game.correct);
    memset(s->game.eligible, 0, sizeof s->game.eligible);
    memset(s->game.wager_set, 0, sizeof s->game.wager_set);
    for (int i = 0; i < JPQ_MAX_PLAYERS; i++) { s->game.players[i].ready = 0; s->game.players[i].score = 0; }
    return 0;
}

/* Applied on the poll thread when the host relaunches the TV surface: a committed
 * quit returns to a clean lobby for a new game, while an interrupted (paused)
 * game is left paused so it can simply be resumed. Either way the quit and
 * armed-confirm flags are cleared, so the renderer can never start on a stale
 * quit view and abort immediately. */
static void service_relaunch(JpqService *s) {
    if (s->game.quit || s->quit) {
        do_reset_lobby(s);
    } else {
        s->game.quit = 0;
        s->game.quit_confirm = 0;
        s->game.restart_confirm = 0;
    }
    s->quit = 0;
    s->dirty_save = s->dirty_broadcast = 1;
    update_window(s);
}

#define JPQ_SEQ_MAX 0x7FFFFFFFLL  /* reject absurd / wrap-prone sequences */

/* Strict JSON integer: must be J_NUM, integral (no fraction), finite and within
 * [lo,hi]. Returns 0 and stores the value in *out on success. */
static int int_field(struct jval *o, const char *k, int64_t lo, int64_t hi, int64_t *out) {
    struct jval *v = jget(o, k);
    if (!v || v->t != J_NUM) return -1;
    double d = v->num;
    if (!(d > -9.0e18 && d < 9.0e18)) return -1;  /* keep the cast defined */
    int64_t iv = (int64_t)d;
    if (!(d == (double)iv)) return -1;      /* fractional */
    if (iv < lo || iv > hi) return -1;
    *out = iv;
    return 0;
}

static int apply_op(JpqService *s, int role, uint32_t player_id, struct jval *m, int trusted,
                    uint64_t now, char *err, size_t errsz) {
    (void)now;
    if (s->save_failed) { snprintf(err, errsz, "storage unavailable"); return -1; }
    const char *action = sval(m, "action");
    if (!action) { snprintf(err, errsz, "missing action"); return -1; }

    int service = is_service_action(action);
    int code = service ? -1 : action_code(action);
    if (!service && code < 0) { snprintf(err, errsz, "unknown action"); return -1; }

    /* Installed LAN runs refuse to advance gameplay while the TV surface is
     * offline, so controllers can never drive a game the room cannot see. */
    if (!service && __atomic_load_n(&s->native_gate, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&s->native_active, __ATOMIC_ACQUIRE)) {
        snprintf(err, errsz, "native display offline");
        return -1;
    }

    uint8_t actor;
    int slot;
    if (role == JPQ_ROLE_HOST) { actor = JPQ_HOST; slot = 6; }
    else {
        slot = slot_by_id(s, player_id);
        if (slot < 0) { snprintf(err, errsz, "not in game"); return -1; }
        actor = (uint8_t)slot;
    }

    /* Envelope. Every UNTRUSTED command must carry an explicit integer epoch
     * and sequence; only the private native IPC channel may autofill them. The
     * sequence must be nonzero, strictly increasing and within range so it can
     * never wrap back onto an already-accepted value. */
    uint32_t seq;
    if (trusted) {
        seq = s->game.last_seq[slot] + 1;
        if (seq == 0) seq = 1;
    } else {
        int64_t e;
        if (int_field(m, "epoch", 0, 0xFFFFFFFFLL, &e)) { snprintf(err, errsz, "missing or invalid epoch"); return -1; }
        if ((uint32_t)e != s->game.epoch) { snprintf(err, errsz, "stale epoch"); return -1; }
        int64_t sq;
        if (int_field(m, "seq", 1, JPQ_SEQ_MAX, &sq)) { snprintf(err, errsz, "missing or invalid sequence"); return -1; }
        seq = (uint32_t)sq;
        if (seq <= s->game.last_seq[slot]) { snprintf(err, errsz, "replayed sequence"); return -1; }
    }

    if (service) {
        if (role != JPQ_ROLE_HOST) { snprintf(err, errsz, "host only"); return -1; }
        int rc;
        if (!strcmp(action, "CONFIG")) rc = do_config(s, m);
        else if (!strcmp(action, "FILTER")) rc = do_filter(s, m);
        else if (!strcmp(action, "REMOVE")) rc = do_remove(s, (uint32_t)ival(m, "target", 0), err, errsz);
        else rc = do_reset_lobby(s);
        if (rc) { if (!err[0]) snprintf(err, errsz, "action failed"); return -1; }
        if (s->game.last_seq[slot] < seq) s->game.last_seq[slot] = seq;
        s->game.revision++;
        update_window(s);
        s->dirty_save = s->dirty_broadcast = 1;
        return 0;
    }

    /* The clue generation / open-window id is mandatory (and must match) for
     * every untrusted action that depends on which clue/window is open: a late
     * buzz, wager, answer, judge or pass after a rebuzz or a new clue, and a
     * board selection made with an old window. Trusted native may autofill. */
    int window_sensitive = code == JPQ_ACT_BUZZ || code == JPQ_ACT_SUBMIT || code == JPQ_ACT_WAGER ||
                           code == JPQ_ACT_JUDGE || code == JPQ_ACT_PASS || code == JPQ_ACT_SELECT_CLUE;
    if (window_sensitive && !trusted) {
        int64_t w;
        if (int_field(m, "window", 0, 0xFFFFFFFFLL, &w)) { snprintf(err, errsz, "missing or invalid window"); return -1; }
        if ((uint32_t)w != s->window) { snprintf(err, errsz, "stale clue"); return -1; }
    }

    uint8_t target = JPQ_NOBODY;
    if (code == JPQ_ACT_SELECT_CLUE) {
        int64_t t = ival(m, "target", -1);
        if (t < 0 || t >= JPQ_CELLS) { snprintf(err, errsz, "bad clue"); return -1; }
        target = (uint8_t)t;
    } else if (code == JPQ_ACT_JUDGE || code == JPQ_ACT_PASS || code == JPQ_ACT_SCORE) {
        int tslot = slot_by_id(s, (uint32_t)ival(m, "target", 0));
        if (tslot < 0) { snprintf(err, errsz, "unknown player"); return -1; }
        target = (uint8_t)tslot;
    }

    JpqAction a;
    memset(&a, 0, sizeof a);
    a.type = (uint8_t)code;
    a.actor = actor;
    a.target = target;
    a.value = (int32_t)ival(m, "value", 0);
    a.epoch = s->game.epoch;
    /* The revision and timestamp are always stamped by the server; any
     * client-supplied score/revision/timestamp field is ignored. */
    a.revision = s->game.revision;
    a.seq = seq;
    const char *text = sval(m, "text");
    if (text) snprintf(a.text, sizeof a.text, "%s", text);

    if (jpq_apply(&s->game, &a, now)) { snprintf(err, errsz, "%s", s->game.err); return -1; }
    update_window(s);
    s->dirty_save = s->dirty_broadcast = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* HTTP API                                                             */
/* ------------------------------------------------------------------ */

static char *json_str_dup(const char *v) { return strdup(v ? v : ""); }

static char *json_error_dup(const char *msg) {
    struct sb o = {0};
    sb_puts(&o, "{\"ok\":false,\"error\":");
    sb_json_str(&o, msg);
    sb_puts(&o, "}");
    return o.p;
}

static int join_rate_ok(JpqService *s, uint64_t now) {
    if (now - s->join_window > JPQ_JOIN_WINDOW_MS) { s->join_window = now; s->join_attempts = 0; }
    return s->join_attempts < JPQ_JOIN_GLOBAL_MAX;
}

static char *http_join(JpqService *s, const JpqHttpRequest *req, int *status, uint64_t now) {
    struct jval *m = json_parse(req->body && *req->body ? req->body : "{}", req->body_len ? req->body_len : 2);
    if (!m || m->t != J_OBJ) { jfree(m); *status = 400; return json_error_dup("invalid body"); }

    const char *token = sval(m, "token");
    if (token && *token) {
        int si = session_by_token(s, token);
        if (s->host_token[0] && tok_eq(s->host_token, token)) {
            jfree(m);
            struct sb o = {0};
            sb_puts(&o, "{\"ok\":true,\"role\":\"host\",\"token\":");
            sb_json_str(&o, s->host_token);
            sb_fmt(&o, ",\"seq\":%u}", s->game.last_seq[6]);
            return o.p;
        }
        if (si < 0) { jfree(m); *status = 401; return json_error_dup("invalid token"); }
        jfree(m);
        struct sb o = {0};
        sb_puts(&o, "{\"ok\":true,\"role\":\"player\",\"playerId\":");
        sb_fmt(&o, "%u,\"color\":", s->sessions[si].id);
        sb_json_str(&o, s->sessions[si].color);
        sb_puts(&o, ",\"token\":");
        sb_json_str(&o, s->sessions[si].token);
        int slot = slot_by_id(s, s->sessions[si].id);
        sb_fmt(&o, ",\"seq\":%u}", slot >= 0 ? s->game.last_seq[slot] : 0);
        return o.p;
    }

    if (!join_rate_ok(s, now)) { jfree(m); *status = 429; return json_error_dup("too many join attempts"); }
    const char *pin = sval(m, "pin");
    if (!pin || !pin_eq(pin, s->join_pin)) {
        s->join_attempts++;
        jfree(m);
        *status = 401;
        return json_error_dup("invalid join pin");
    }
    if (s->game.phase != JPQ_PHASE_LOBBY) { jfree(m); *status = 409; return json_error_dup("game in progress"); }
    if (s->game.player_count >= JPQ_MAX_PLAYERS) { jfree(m); *status = 409; return json_error_dup("game full"); }

    const char *name = sval(m, "name");
    if (!name || !*name) { jfree(m); *status = 400; return json_error_dup("name required"); }
    if (strlen(name) > JPQ_NAME_MAX - 1) { jfree(m); *status = 400; return json_error_dup("name too long"); }
    if (name_taken(s, name, 0)) { jfree(m); *status = 409; return json_error_dup("name taken"); }

    const char *want = sval(m, "color");
    const char *color = NULL;
    if (want && *want) {
        for (int i = 0; i < 6; i++) if (!strcmp(want, JPQ_PALETTE[i])) color = JPQ_PALETTE[i];
        if (!color) { jfree(m); *status = 400; return json_error_dup("unknown color"); }
        if (color_taken(s, color, 0)) { jfree(m); *status = 409; return json_error_dup("color taken"); }
    } else {
        for (int i = 0; i < 6; i++) if (!color_taken(s, JPQ_PALETTE[i], 0)) { color = JPQ_PALETTE[i]; break; }
        if (!color) { jfree(m); *status = 409; return json_error_dup("no colors left"); }
    }

    uint32_t id = s->next_player_id++;
    if (!id) id = s->next_player_id++;
    int si = -1;
    for (int i = 0; i < JPQ_MAX_PLAYERS; i++) if (!s->sessions[i].used) { si = i; break; }
    if (si < 0 || jpq_add_player(&s->game, name, id) < 0) { jfree(m); *status = 409; return json_error_dup("cannot join"); }
    Session *se = &s->sessions[si];
    memset(se, 0, sizeof *se);
    se->used = 1;
    se->id = id;
    snprintf(se->color, sizeof se->color, "%s", color);
    if (make_token(se->token)) { jfree(m); *status = 500; return json_error_dup("entropy unavailable"); }
    int slot = slot_by_id(s, id);
    if (slot >= 0) s->game.players[slot].connected = 0;
    jfree(m);
    s->dirty_save = s->dirty_broadcast = 1;
    struct sb o = {0};
    sb_fmt(&o, "{\"ok\":true,\"role\":\"player\",\"playerId\":%u,\"color\":", id);
    sb_json_str(&o, color);
    sb_puts(&o, ",\"token\":");
    sb_json_str(&o, se->token);
    sb_fmt(&o, ",\"seq\":%u}", slot >= 0 ? s->game.last_seq[slot] : 0);
    return o.p;
}

static char *http_host(JpqService *s, const JpqHttpRequest *req, int *status, uint64_t now) {
    struct jval *m = json_parse(req->body && *req->body ? req->body : "{}", req->body_len ? req->body_len : 2);
    if (!m || m->t != J_OBJ) { jfree(m); *status = 400; return json_error_dup("invalid body"); }
    if (!join_rate_ok(s, now)) { jfree(m); *status = 429; return json_error_dup("too many attempts"); }
    const char *pin = sval(m, "pin");
    if (!pin || !pin_eq(pin, s->host_pin)) {
        s->join_attempts++;
        jfree(m);
        *status = 401;
        return json_error_dup("invalid host pin");
    }
    jfree(m);
    struct sb o = {0};
    sb_puts(&o, "{\"ok\":true,\"role\":\"host\",\"token\":");
    sb_json_str(&o, s->host_token);
    sb_fmt(&o, ",\"seq\":%u}", s->game.last_seq[6]);
    return o.p;
}

char *jpq_service_http(void *ud, const JpqHttpRequest *req, size_t *len, const char **ctype, int *status) {
    JpqService *s = ud;
    uint64_t now = ms_now();
    service_pretick(s, now);
    *ctype = "application/json";
    *status = 200;
    if (!strcmp(req->method, "GET") && (!strcmp(req->path, "/") || !strcmp(req->path, "/index.html"))) {
        *ctype = "text/html; charset=utf-8";
        size_t n = JPQ_WEB_HTML_LEN;
        char *buf = malloc(n + 1);
        if (!buf) { *status = 500; char *e = json_error_dup("out of memory"); *len = strlen(e); return e; }
        memcpy(buf, JPQ_WEB_HTML, n);
        buf[n] = 0;
        *len = n;
        return buf;
    }
    if (!strcmp(req->method, "GET") && !strcmp(req->path, "/health")) {
        char *r = json_str_dup("{\"ok\":true,\"service\":\"jeopardy\",\"capacity\":6}");
        *len = strlen(r);
        return r;
    }
    if (!strcmp(req->method, "GET") && !strcmp(req->path, "/api/state")) {
        char token[128];
        int role = JPQ_ROLE_PUBLIC;
        uint32_t pid = 0;
        if (query_param(req->query, "token", token, sizeof token) && *token) {
            if (tok_eq(s->host_token, token)) role = JPQ_ROLE_HOST;
            else {
                int si = session_by_token(s, token);
                if (si < 0) { *status = 401; char *e = json_error_dup("invalid token"); *len = strlen(e); return e; }
                role = JPQ_ROLE_PLAYER;
                pid = s->sessions[si].id;
            }
        }
        char *msg = state_message(s, role, pid, now, len);
        if (!msg) { *status = 500; char *e = json_error_dup("snapshot failed"); *len = strlen(e); return e; }
        return msg;
    }
    if (!strcmp(req->method, "POST") && !strcmp(req->path, "/api/join")) {
        char *r = http_join(s, req, status, now);
        *len = strlen(r);
        service_flush(s, now);
        return r;
    }
    if (!strcmp(req->method, "POST") && !strcmp(req->path, "/api/host")) {
        char *r = http_host(s, req, status, now);
        *len = strlen(r);
        return r;
    }
    *status = 404;
    char *e = json_error_dup("not found");
    *len = strlen(e);
    return e;
}

/* ------------------------------------------------------------------ */
/* WebSocket protocol                                                   */
/* ------------------------------------------------------------------ */

int jpq_service_ws_path(void *ud, const JpqHttpRequest *req) {
    (void)ud;
    return !strcmp(req->path, "/ws");
}

void jpq_service_ws_open(void *ud, JpqConnId conn) {
    JpqService *s = ud;
    for (int i = 0; i < JPQ_MAX_VIEWERS; i++) {
        if (s->viewers[i].used) continue;
        memset(&s->viewers[i], 0, sizeof s->viewers[i]);
        s->viewers[i].used = 1;
        s->viewers[i].conn = conn;
        s->viewers[i].role = JPQ_ROLE_UNAUTH;
        struct sb o = {0};
        sb_fmt(&o, "{\"type\":\"welcome\",\"protocol\":1,\"capacity\":%d,\"joinPinRequired\":true}", JPQ_MAX_PLAYERS);
        if (o.p) send_to(s, conn, o.p, o.len);
        free(o.p);
        return;
    }
    if (s->closef) s->closef(s->netud, conn, 1013);
}

static void become_role(JpqService *s, Viewer *v, int role, uint32_t pid, uint64_t now) {
    v->role = role;
    v->player_id = pid;
    if (role == JPQ_ROLE_PLAYER) {
        int slot = slot_by_id(s, pid);
        if (slot >= 0) s->game.players[slot].connected = 1;
    }
    s->dirty_broadcast = 1;
    send_state(s, v, now);
    service_flush(s, now);
}

static void ws_hello(JpqService *s, Viewer *v, struct jval *m, uint64_t now) {
    const char *token = sval(m, "token");
    if (!token || !*token) { send_error(s, v->conn, "auth", "missing token"); return; }
    if (s->host_token[0] && tok_eq(s->host_token, token)) { become_role(s, v, JPQ_ROLE_HOST, 0, now); return; }
    int si = session_by_token(s, token);
    if (si < 0) { send_error(s, v->conn, "auth", "invalid token"); return; }
    uint32_t pid = s->sessions[si].id;
    int slot = slot_by_id(s, pid);
    if (slot < 0) { send_error(s, v->conn, "auth", "session not in game"); return; }
    Viewer *other = viewer_by_player(s, pid);
    if (other && other != v && s->closef) { s->closef(s->netud, other->conn, 4000); other->used = 0; }
    become_role(s, v, JPQ_ROLE_PLAYER, pid, now);
}

static void ws_command(JpqService *s, Viewer *v, struct jval *m, uint64_t now) {
    if (v->role == JPQ_ROLE_UNAUTH) { send_error(s, v->conn, "auth", "not authenticated"); return; }
    if (v->role == JPQ_ROLE_PUBLIC) { send_error(s, v->conn, "role", "spectators cannot act"); return; }
    char err[192];
    err[0] = 0;
    if (apply_op(s, v->role, v->player_id, m, 0, now, err, sizeof err)) {
        send_error(s, v->conn, "rejected", err[0] ? err : "rejected");
        return;
    }
    service_flush(s, now);
}

void jpq_service_ws_text(void *ud, JpqConnId conn, const char *data, size_t len) {
    JpqService *s = ud;
    Viewer *v = viewer_by_conn(s, conn);
    if (!v) return;
    uint64_t now = ms_now();
    service_pretick(s, now);
    struct jval *m = json_parse(data, len);
    if (!m || m->t != J_OBJ) { jfree(m); send_error(s, conn, "bad", "invalid message"); return; }
    const char *type = sval(m, "type");
    if (!type) send_error(s, conn, "bad", "missing type");
    else if (!strcmp(type, "hello")) ws_hello(s, v, m, now);
    else if (!strcmp(type, "spectate")) { become_role(s, v, JPQ_ROLE_PUBLIC, 0, now); }
    else if (!strcmp(type, "cmd") || !strcmp(type, "action")) ws_command(s, v, m, now);
    else if (!strcmp(type, "join")) {
        JpqHttpRequest req;
        memset(&req, 0, sizeof req);
        req.conn = conn;
        req.body = data;
        req.body_len = len;
        int status = 200;
        char *r = http_join(s, &req, &status, now);
        if (status != 200) { send_error(s, conn, "join", "join rejected"); free(r); }
        else {
            /* re-parse for the issued identity */
            struct jval *again = json_parse(r, strlen(r));
            uint32_t pid = (uint32_t)jnum(jget(again, "playerId"), 0);
            jfree(again);
            send_to(s, conn, r, strlen(r));
            become_role(s, v, JPQ_ROLE_PLAYER, pid, now);
            free(r);
        }
    }
    else if (!strcmp(type, "host")) {
        JpqHttpRequest req;
        memset(&req, 0, sizeof req);
        req.conn = conn;
        req.body = data;
        req.body_len = len;
        int status = 200;
        char *r = http_host(s, &req, &status, now);
        if (status != 200) { send_error(s, conn, "auth", "host rejected"); free(r); }
        else { send_to(s, conn, r, strlen(r)); free(r); become_role(s, v, JPQ_ROLE_HOST, 0, now); }
    }
    else if (!strcmp(type, "ping")) send_to(s, conn, "{\"type\":\"pong\"}", 17);
    else send_error(s, conn, "bad", "unknown message type");
    jfree(m);
}

void jpq_service_ws_close(void *ud, JpqConnId conn, int code) {
    (void)code;
    JpqService *s = ud;
    Viewer *v = viewer_by_conn(s, conn);
    if (!v) return;
    if (v->role == JPQ_ROLE_PLAYER) {
        int slot = slot_by_id(s, v->player_id);
        if (slot >= 0) { s->game.players[slot].connected = 0; s->dirty_save = s->dirty_broadcast = 1; }
    }
    v->used = 0;
    service_flush(s, ms_now());
}

/* ------------------------------------------------------------------ */
/* Private native command channel                                       */
/* ------------------------------------------------------------------ */

/* Reply on the private native line channel so the renderer can surface the
 * outcome (bounded, best-effort: a slow/full peer never blocks the loop). */
static void line_reply(JpqService *s, JpqConnId conn, int ok, const char *err) {
    struct sb o = {0};
    if (ok) sb_puts(&o, "{\"ok\":true}");
    else {
        sb_puts(&o, "{\"ok\":false,\"error\":");
        sb_json_str(&o, err && *err ? err : "rejected");
        sb_puts(&o, "}");
    }
    if (o.p) send_to(s, conn, o.p, o.len);
    free(o.p);
}

void jpq_service_line(void *ud, JpqConnId conn, const char *line, size_t len) {
    JpqService *s = ud;
    uint64_t now = ms_now();
    service_pretick(s, now);
    struct jval *m = json_parse(line, len);
    if (!m || m->t != J_OBJ) { jfree(m); line_reply(s, conn, 0, "invalid command"); return; }
    struct jval *actor = jget(m, "actor");
    int role = -1;
    uint32_t pid = 0;
    if (actor && actor->t == J_STR && !strcmp(actor->str, "host")) role = JPQ_ROLE_HOST;
    else if (actor && actor->t == J_NUM) {
        pid = (uint32_t)actor->num;
        if (slot_by_id(s, pid) >= 0) role = JPQ_ROLE_PLAYER;
    }
    if (role < 0) { jfree(m); line_reply(s, conn, 0, "invalid actor"); return; }
    char err[192];
    err[0] = 0;
    if (apply_op(s, role, pid, m, 1, now, err, sizeof err) == 0) {
        service_flush(s, now);
        line_reply(s, conn, 1, NULL);
    } else {
        line_reply(s, conn, 0, err);
    }
    jfree(m);
}

/* ------------------------------------------------------------------ */
/* Tick / lifecycle                                                     */
/* ------------------------------------------------------------------ */

void jpq_service_tick(void *ud, uint64_t now) {
    JpqService *s = ud;
    int relaunch = __atomic_exchange_n(&s->relaunch_request, 0, __ATOMIC_SEQ_CST);
    if (__atomic_exchange_n(&s->pause_request, 0, __ATOMIC_SEQ_CST) &&
        s->game.phase != JPQ_PHASE_LOBBY && !s->game.paused) {
        JpqAction a;
        memset(&a, 0, sizeof a);
        a.type = JPQ_ACT_PAUSE;
        a.actor = JPQ_HOST;
        a.target = JPQ_NOBODY;
        a.epoch = s->game.epoch;
        a.revision = s->game.revision;
        a.seq = s->game.last_seq[6] + 1;
        if (!jpq_apply(&s->game, &a, now)) {
            s->game.last_seq[6] = a.seq;
            s->dirty_save = s->dirty_broadcast = 1;
            update_window(s);
        }
    }
    if (relaunch) service_relaunch(s);
    service_pretick(s, now);
    service_flush(s, now);
    /* The ack is published only after the rewritten public display has been
     * flushed, so a caller that waits on it launches the renderer against a
     * fresh, non-quit view. */
    if (relaunch) __atomic_store_n(&s->relaunch_ack, 1, __ATOMIC_SEQ_CST);
}

void jpq_service_request_pause(JpqService *s) {
    if (s) __atomic_store_n(&s->pause_request, 1, __ATOMIC_SEQ_CST);
}

void jpq_service_request_relaunch(JpqService *s) {
    if (!s) return;
    __atomic_store_n(&s->relaunch_ack, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s->relaunch_request, 1, __ATOMIC_SEQ_CST);
}

int jpq_service_relaunch_done(JpqService *s) {
    return s ? __atomic_load_n(&s->relaunch_ack, __ATOMIC_SEQ_CST) : 1;
}

void jpq_service_set_native_gate(JpqService *s, int gate) {
    if (s) __atomic_store_n(&s->native_gate, gate ? 1 : 0, __ATOMIC_SEQ_CST);
}

void jpq_service_set_native_active(JpqService *s, int active) {
    if (s) __atomic_store_n(&s->native_active, active ? 1 : 0, __ATOMIC_SEQ_CST);
}

const char *jpq_service_command_socket(JpqService *s) { return s->command_sock; }
int jpq_service_quit(JpqService *s) { return s ? (s->quit || s->game.quit) : 0; }

/* Save the live timer remainder before the service is torn down. */
void jpq_service_shutdown(JpqService *s) {
    if (!s) return;
    service_save(s, ms_now());
}

int jpq_service_start(JpqService *s, const char *data_dir, const char *db_path) {
    memset(s, 0, sizeof *s);
    snprintf(s->data_dir, sizeof s->data_dir, "%s", data_dir);
    snprintf(s->db_path, sizeof s->db_path, "%s", db_path);
    snprintf(s->command_sock, sizeof s->command_sock, "%s/native/command.sock", data_dir);
    if (jpq_db_load(&s->db, db_path)) return -1;

    JpqConfig cfg;
    jpq_config_default(&cfg);
    jpq_init(&s->game, &cfg);
    jpq_attach(&s->game, &s->db, &s->hist);
    if (jpq_history_init(&s->hist, s->db.clue_count)) return -1;
    char hpath[1200];
    if (!svc_path(s, hpath, sizeof hpath, "jeopardy-history.bin")) jpq_history_load(&s->hist, hpath);

    load_sessions(s);
    if (jpq_load(&s->game, data_dir)) {
        jpq_init(&s->game, &cfg);
        jpq_attach(&s->game, &s->db, &s->hist);
    }
    for (int i = 0; i < JPQ_MAX_PLAYERS; i++) s->game.players[i].connected = 0;

    if (!s->host_token[0] && make_token(s->host_token)) return -1;
    for (int i = 0; i < s->game.player_count; i++) {
        uint32_t id = s->game.players[i].id;
        if (session_by_id(s, id) >= 0) continue;
        int si = -1;
        for (int k = 0; k < JPQ_MAX_PLAYERS; k++) if (!s->sessions[k].used) { si = k; break; }
        if (si < 0) continue;
        Session *se = &s->sessions[si];
        memset(se, 0, sizeof *se);
        se->used = 1;
        se->id = id;
        snprintf(se->color, sizeof se->color, "%s", JPQ_PALETTE[i % 6]);
        if (make_token(se->token)) return -1;
    }
    if (s->next_player_id < 1) s->next_player_id = 1;
    for (int i = 0; i < s->game.player_count; i++)
        if (s->game.players[i].id >= s->next_player_id) s->next_player_id = s->game.players[i].id + 1;

    if (make_pin(s->join_pin, 4) || make_pin(s->host_pin, 6)) {
        fprintf(stderr, "jeopardy: entropy unavailable; refusing to start\n");
        return -1;
    }
    char nd[1200];
    native_dir(s, nd, sizeof nd);
    update_window(s);
    return 0;
}

int jpq_service_set_port(JpqService *s, int port, const char *public_url) {
    s->port = port;
    if (public_url && *public_url) snprintf(s->public_url, sizeof s->public_url, "%s", public_url);
    else snprintf(s->public_url, sizeof s->public_url, "http://127.0.0.1:%d/", port);
    int rc = write_metadata(s);
    write_public_display(s, ms_now());
    return rc;
}

void jpq_service_set_net(JpqService *s, JpqSendFn send, JpqCloseFn closef, void *ud) {
    s->send = send;
    s->closef = closef;
    s->netud = ud;
}

JpqService *jpq_service_create(void) {
    JpqService *s = calloc(1, sizeof *s);
    return s;
}

void jpq_service_destroy(JpqService *s) {
    if (!s) return;
    jpq_db_free(&s->db);
    jpq_history_free(&s->hist);
    free(s);
}
