/* Receiver-native Jellyfin service for the DIRECTV HR54.
 *
 * Everything the TV frontend needs runs on the receiver itself: Quick Connect
 * auth state, browse/search, artwork, PlaybackInfo, the opaque MPEG-TS relay,
 * transport control, and receiver UI presentation.  The only external service
 * required is the Jellyfin server.  The old host backend (server.py) is the
 * behavioral specification, not a dependency.
 *
 * Build: zig cc -target mips-linux-musleabi -mcpu=mips32 -static -O2 \
 *          -o hr54-jf hr54_jf.c
 * Usage: hr54-jf DOCROOT JELLYFIN_IPV4 JELLYFIN_PORT LISTEN_PORT [--no-launcher]
 *
 * Layout under /var/hr54-persist/jellyfin (or $JF_PERSIST_ROOT for tests):
 *   config/config.json, config/token   0600, directories 0700
 *   state/tv-state.json                saved TV browse checkpoint
 *   cache/<itemId> + cache/<itemId>.ct artwork cache
 *   log/jf.log
 */

#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <unistd.h>
#include "../../tools/native-ui/presentation_policy.h"

/* ------------------------------------------------------------------ */
/* Tunables mirrored from the reference server.py                     */
/* ------------------------------------------------------------------ */

#define JF_PERSIST_DEFAULT "/var/hr54-persist/jellyfin"
#define JF_PLAY_CMD_DEFAULT "/var/opt/hr54/bin/hr54-play-url"
#define JF_SHEF_PORT 8080

#define LIVE_TV_SCREEN 2320
#define MENU_SCREEN_A 10306
#define MENU_SCREEN_B 2100
/* MENU is the custom top-level menu trigger, so the stock Druid menu is no
 * longer a presentation that must be left alone.  These three delays decide
 * how long the stock menu is visible before itvStartApp; each one is paired
 * with an authoritative screen-state probe, so they are the only thing
 * standing between a MENU press and the custom page.  Keep them small. */
#define MENU_IDLE_SECONDS 0.25
#define WATCH_POLL 0.25
#define WALK_SETTLE 0.25
/* Wall-clock ceiling for the walk, not a fixed iteration count: a short
 * settle means more polls fit inside the same teardown window. */
#define WALK_BUDGET 6.0
#define WALK_MAX_STEPS 24
#define REMOTE_EXIT_IDLE 2.0
#define REMOTE_EXIT_DISMISS 1.5
#define APP_EXIT_GRACE 15.0
#define LAUNCH_COOLDOWN 30.0
#define POST_STOP_DISMISS 8.0
#define RESUME_RESTART 20.0
#define QC_LIFETIME 600.0
#define CLAIM_WAIT 15.0

#define JF_STREAM_SLOTS 4
#define RELAY_CHUNK 65536
#define REQ_CAP 16384
#define BODY_CAP 8192
#define RESP_CAP (4 * 1024 * 1024)

static const char *persist_root = JF_PERSIST_DEFAULT;
static const char *play_cmd = JF_PLAY_CMD_DEFAULT;
static const char *docroot = "";
/* Native mode is explicit: legacy maintenance pages retain their behavior. */
static int native_frontend;

/* ------------------------------------------------------------------ */
/* Error plumbing: request children are single-threaded.              */
/* ------------------------------------------------------------------ */

static char g_err[256];

static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return -1;
}

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void nap(double seconds) {
    struct timespec ts = {(time_t)seconds,
                          (long)((seconds - (time_t)seconds) * 1e9)};
    nanosleep(&ts, NULL);
}

static int write_all_fd(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Growable string buffer                                              */
/* ------------------------------------------------------------------ */

struct sb {
    char *p;
    size_t len, cap;
};

static int sb_grow(struct sb *b, size_t need) {
    if (b->p && b->len + need + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + need + 1) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) return fail("out of memory");
    b->p = p;
    b->cap = cap;
    return 0;
}

static int sb_putn(struct sb *b, const char *s, size_t n) {
    if (sb_grow(b, n)) return -1;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
    return 0;
}

static int sb_puts(struct sb *b, const char *s) { return sb_putn(b, s, strlen(s)); }

static int sb_fmt(struct sb *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n < sizeof tmp) return sb_putn(b, tmp, (size_t)n);
    char *big = malloc((size_t)n + 1);
    if (!big) return fail("out of memory");
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int rc = sb_putn(b, big, (size_t)n);
    free(big);
    return rc;
}

/* JSON string literal with escaping (quote, backslash, C0 controls). */
static int sb_json_str(struct sb *b, const char *s) {
    if (sb_puts(b, "\"")) return -1;
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': if (sb_puts(b, "\\\"")) return -1; break;
        case '\\': if (sb_puts(b, "\\\\")) return -1; break;
        case '\b': if (sb_puts(b, "\\b")) return -1; break;
        case '\f': if (sb_puts(b, "\\f")) return -1; break;
        case '\n': if (sb_puts(b, "\\n")) return -1; break;
        case '\r': if (sb_puts(b, "\\r")) return -1; break;
        case '\t': if (sb_puts(b, "\\t")) return -1; break;
        default:
            if (c < 0x20) { if (sb_fmt(b, "\\u%04x", c)) return -1; }
            else if (sb_putn(b, (const char *)&c, 1)) return -1;
        }
    }
    return sb_puts(b, "\"");
}

/* ------------------------------------------------------------------ */
/* URL encoding helpers                                                */
/* ------------------------------------------------------------------ */

static int url_encode(struct sb *out, const char *s) {
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (sb_putn(out, (const char *)&c, 1)) return -1;
        } else if (sb_fmt(out, "%%%02X", c)) {
            return -1;
        }
    }
    return 0;
}

static size_t url_decode(char *dst, size_t dstsz, const char *src, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < dstsz; i++) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < n && isxdigit((unsigned char)src[i + 1]) &&
                   isxdigit((unsigned char)src[i + 2])) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            c = (char)strtol(hex, NULL, 16);
            i += 2;
        }
        dst[o++] = c;
    }
    dst[o] = 0;
    return o;
}

/* Extract one query parameter (percent-decoded) from "a=b&c=d". */
static int query_param(const char *query, const char *name,
                       char *out, size_t outsz) {
    if (!query) return 0;
    size_t nlen = strlen(name);
    const char *p = query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', seg);
        if (eq && (size_t)(eq - p) == nlen && !memcmp(p, name, nlen)) {
            url_decode(out, outsz, eq + 1, seg - nlen - 1);
            return 1;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Minimal JSON DOM parser                                             */
/* ------------------------------------------------------------------ */

enum jtype { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };

struct jval {
    enum jtype t;
    double num;
    char *str;
    struct jval **items;
    char **keys;
    size_t n, cap;
};

static void jfree(struct jval *v) {
    if (!v) return;
    free(v->str);
    for (size_t i = 0; i < v->n; i++) {
        jfree(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v);
}

static int jpush(struct jval *parent, char *key, struct jval *child) {
    if (parent->n == parent->cap) {
        size_t cap = parent->cap ? parent->cap * 2 : 8;
        struct jval **items = realloc(parent->items, cap * sizeof *items);
        if (!items) return -1;
        parent->items = items;
        if (parent->t == J_OBJ) {
            char **keys = realloc(parent->keys, cap * sizeof *keys);
            if (!keys) return -1;
            parent->keys = keys;
        }
        parent->cap = cap;
    }
    parent->items[parent->n] = child;
    if (parent->keys) parent->keys[parent->n] = key;
    parent->n++;
    return 0;
}

static void jskip_ws(const char **p, const char *end) {
    while (*p < end && (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r'))
        (*p)++;
}

static struct jval *jparse_value(const char **p, const char *end);

static int jparse_hex4(const char **p, const char *end, unsigned *out) {
    if (end - *p < 4) return -1;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = (*p)[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *p += 4;
    *out = v;
    return 0;
}

static int utf8_emit(struct sb *b, unsigned cp) {
    char t[4];
    int n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) {
        t[0] = (char)(0xC0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        t[0] = (char)(0xF0 | (cp >> 18));
        t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    return sb_putn(b, t, (size_t)n);
}

/* Parse a JSON string body (opening quote at **p) into a raw buffer. */
static int jparse_string(struct sb *out, const char **p, const char *end) {
    if (*p >= end || **p != '"') return -1;
    (*p)++;
    while (*p < end) {
        char c = **p;
        if (c == '"') { (*p)++; return 0; }
        if ((unsigned char)c < 0x20) return -1;
        if (c == '\\') {
            (*p)++;
            if (*p >= end) return -1;
            char e = **p;
            (*p)++;
            switch (e) {
            case '"': if (sb_putn(out, "\"", 1)) return -1; break;
            case '\\': if (sb_putn(out, "\\", 1)) return -1; break;
            case '/': if (sb_putn(out, "/", 1)) return -1; break;
            case 'b': if (sb_putn(out, "\b", 1)) return -1; break;
            case 'f': if (sb_putn(out, "\f", 1)) return -1; break;
            case 'n': if (sb_putn(out, "\n", 1)) return -1; break;
            case 'r': if (sb_putn(out, "\r", 1)) return -1; break;
            case 't': if (sb_putn(out, "\t", 1)) return -1; break;
            case 'u': {
                unsigned cp;
                if (jparse_hex4(p, end, &cp)) return -1;
                if (cp >= 0xD800 && cp <= 0xDBFF && end - *p >= 6 &&
                    (*p)[0] == '\\' && (*p)[1] == 'u') {
                    const char *save = *p;
                    unsigned lo;
                    *p += 2;
                    if (!jparse_hex4(p, end, &lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else
                        *p = save;
                }
                if (utf8_emit(out, cp)) return -1;
                break;
            }
            default:
                return -1;
            }
        } else {
            if (sb_putn(out, &c, 1)) return -1;
            (*p)++;
        }
    }
    return -1;
}

static struct jval *jnew(enum jtype t) {
    struct jval *v = calloc(1, sizeof *v);
    if (v) v->t = t;
    return v;
}

static struct jval *jparse_value(const char **p, const char *end) {
    jskip_ws(p, end);
    if (*p >= end) return NULL;
    char c = **p;
    if (c == '{') {
        (*p)++;
        struct jval *o = jnew(J_OBJ);
        if (!o) return NULL;
        jskip_ws(p, end);
        if (*p < end && **p == '}') { (*p)++; return o; }
        for (;;) {
            jskip_ws(p, end);
            if (*p >= end || **p != '"') { jfree(o); return NULL; }
            struct sb key = {0};
            if (jparse_string(&key, p, end)) { free(key.p); jfree(o); return NULL; }
            jskip_ws(p, end);
            if (*p >= end || **p != ':') { free(key.p); jfree(o); return NULL; }
            (*p)++;
            struct jval *child = jparse_value(p, end);
            if (!child || jpush(o, key.p, child)) {
                free(key.p);
                jfree(child);
                jfree(o);
                return NULL;
            }
            jskip_ws(p, end);
            if (*p < end && **p == ',') { (*p)++; continue; }
            if (*p < end && **p == '}') { (*p)++; return o; }
            jfree(o);
            return NULL;
        }
    }
    if (c == '[') {
        (*p)++;
        struct jval *a = jnew(J_ARR);
        if (!a) return NULL;
        jskip_ws(p, end);
        if (*p < end && **p == ']') { (*p)++; return a; }
        for (;;) {
            struct jval *child = jparse_value(p, end);
            if (!child || jpush(a, NULL, child)) { jfree(child); jfree(a); return NULL; }
            jskip_ws(p, end);
            if (*p < end && **p == ',') { (*p)++; continue; }
            if (*p < end && **p == ']') { (*p)++; return a; }
            jfree(a);
            return NULL;
        }
    }
    if (c == '"') {
        struct sb s = {0};
        if (jparse_string(&s, p, end)) { free(s.p); return NULL; }
        struct jval *v = jnew(J_STR);
        if (!v) { free(s.p); return NULL; }
        v->str = s.p ? s.p : calloc(1, 1);
        if (!v->str) { jfree(v); return NULL; }
        return v;
    }
    if (end - *p >= 4 && !strncmp(*p, "true", 4)) { *p += 4; return jnew(J_TRUE); }
    if (end - *p >= 5 && !strncmp(*p, "false", 5)) { *p += 5; return jnew(J_FALSE); }
    if (end - *p >= 4 && !strncmp(*p, "null", 4)) { *p += 4; return jnew(J_NULL); }
    if (c == '-' || isdigit((unsigned char)c)) {
        char *stop = NULL;
        char *copy = strndup(*p, (size_t)(end - *p));
        if (!copy) return NULL;
        double d = strtod(copy, &stop);
        size_t used = (size_t)(stop - copy);
        free(copy);
        if (!used) return NULL;
        struct jval *v = jnew(J_NUM);
        if (!v) return NULL;
        v->num = d;
        *p += used;
        return v;
    }
    return NULL;
}

static struct jval *json_parse(const char *s, size_t n) {
    const char *p = s, *end = s + n;
    struct jval *v = jparse_value(&p, end);
    if (!v) return NULL;
    jskip_ws(&p, end);
    if (p != end) { jfree(v); return NULL; }
    return v;
}

static struct jval *jget(struct jval *obj, const char *key) {
    if (!obj || obj->t != J_OBJ) return NULL;
    for (size_t i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key)) return obj->items[i];
    return NULL;
}

static struct jval *jnth(struct jval *arr, size_t i) {
    if (!arr || arr->t != J_ARR || i >= arr->n) return NULL;
    return arr->items[i];
}

static const char *jstr(struct jval *v) { return v && v->t == J_STR ? v->str : NULL; }
static double jnum(struct jval *v, double dflt) {
    return v && v->t == J_NUM ? v->num : dflt;
}
static int jbool(struct jval *v, int dflt) {
    if (!v) return dflt;
    if (v->t == J_TRUE) return 1;
    if (v->t == J_FALSE || v->t == J_NULL) return 0;
    return dflt;
}

/* ------------------------------------------------------------------ */
/* HTTP client                                                         */
/* ------------------------------------------------------------------ */

#include "../../modules/jellyfin/http.inc"

/* Shared state: MAP_SHARED anonymous, serialized with flock           */
/* ------------------------------------------------------------------ */

#define JF_NAME_MAX 256
#define JF_ID_MAX 80
#define JF_URL_MAX 2048

struct stream_slot {
    int in_use, claimed, ready, closed, paused, draining;
    char token[64];
    char upath[JF_URL_MAX]; /* path?query on the Jellyfin server */
    char session[64]; /* Jellyfin PlaySessionId for the upstream transcode */
    double paused_since, paused_total, duration;
};

struct shared {
    char jf_host[64];
    int jf_port;
    char device_id[33];
    char token[512];
    char user_id[JF_ID_MAX];
    char user_name[JF_NAME_MAX];
    int auth_valid;
    int jf_quality_bitrate; /* validated discrete policy; next PlaybackInfo */
    char qc_secret[512];
    char qc_code[16];
    double qc_started;
    int playing;
    double play_duration;
    char play_name[JF_NAME_MAX];
    char play_item[JF_ID_MAX];
    char play_token[64];
    char play_session[64]; /* PlaySessionId of the active Jellyfin playback */
    char stop_pending[64]; /* ended session awaiting a Sessions/Playing/Stopped report */
    int return_to_tv;
    int play_live; /* 1 Frigate, 2 IPTV */
    int iptv_active, iptv_claimed;
    pid_t iptv_worker;
    char iptv_token[33], iptv_url[8193], iptv_ua[2049], iptv_ref[2049], iptv_error[256];
    pid_t yt_updater; double yt_update_check;
    int yt_active,yt_claimed; double yt_duration,yt_claim_at; char yt_token[33],yt_video[8193],yt_audio[8193],yt_error[256];
    int ui_return_source;
    unsigned ui_back_seq;
    off_t ui_key_offset;
    ino_t ui_key_inode;
    off_t setup_key_offset;
    ino_t setup_key_inode;
    double ui_key_until;
    /* Additive session identity: one increment per committed playback
     * session, including replacements while already playing. External API
     * clients ignore it; the native shell uses it to distinguish a new
     * committed session from its own mutating operations. */
    unsigned long play_generation;
    double play_started;
    int play_base;
    struct stream_slot streams[JF_STREAM_SLOTS];
    int loopback_failed;
    char lan_addr[64];
    int listen_port;
    /* Observability for the DOOM frame path: how many frames the TV
       frontend has actually pulled, and how many were not-modified replies.
       Lives in shared memory because every request is a separate process. */
    unsigned long doom_frames_served;
    unsigned long doom_frames_304;
};

static struct shared *S;
static int lock_fd_local = -1; /* per-process: separate OFDs make flock exclude */

static void state_lock(void) {
    if (lock_fd_local < 0) {
        char path[512];
        snprintf(path, sizeof path, "%s/state.lock", persist_root);
        lock_fd_local = open(path, O_RDWR | O_CREAT, 0600);
        if (lock_fd_local < 0) lock_fd_local = open("/dev/null", O_RDONLY);
    }
    if (lock_fd_local >= 0)
        while (flock(lock_fd_local, LOCK_EX) && errno == EINTR) {}
}

static void state_unlock(void) {
    if (lock_fd_local >= 0)
        while (flock(lock_fd_local, LOCK_UN) && errno == EINTR) {}
}

static void jf_log_va(int millis, const char *tag, const char *fmt, va_list ap) {
    char path[512], line[512];
    snprintf(path, sizeof path, "%s/log/jf.log", persist_root);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char stamp[40];
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &tm);
    vsnprintf(line, sizeof line, fmt, ap);
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd >= 0) {
        char out[640];
        int n = millis ? snprintf(out, sizeof out, "%s.%03ld %s %s\n", stamp,
                                  ts.tv_nsec / 1000000L, tag, line)
                      : snprintf(out, sizeof out, "%s %s\n", stamp, line);
        if (n > 0) write_all_fd(fd, out, (size_t)n);
        close(fd);
    }
}

static void jf_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    jf_log_va(0, NULL, fmt, ap);
    va_end(ap);
}

/* Menu takeover happens inside a second, so transition milestones need
 * millisecond stamps to be measurable.  jf_log_ms keeps the existing
 * one-second line format untouched and adds a TRANSIT marker instead. */
static void jf_log_ms(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    jf_log_va(1, "TRANSIT", fmt, ap);
    va_end(ap);
}

/* Remote telemetry, logged so the physical remote's key codes can be
 * identified from evidence rather than assumption.  Keydown events carry the
 * vendor DOM key code, which is how a button that Druid does not consume can
 * be told apart from ordinary navigation. */
static void tv_event_record(struct jval *payload) {
    if (!payload || payload->t != J_OBJ) return;
    const char *type = jstr(jget(payload, "type"));
    if (!type) return;
    char path[512];
    snprintf(path, sizeof path, "%s/log/tv-events.log", persist_root);
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd < 0) return;
    char line[512];
    int n;
    if (!strcmp(type, "keydown") || !strcmp(type, "keyup")) {
        const char *key = jstr(jget(payload, "key"));
        n = snprintf(line, sizeof line,
                     "type=%s keyCode=%d which=%d key=%s\n", type,
                     (int)jnum(jget(payload, "keyCode"), 0),
                     (int)jnum(jget(payload, "which"), 0),
                     key ? key : "");
    } else {
        n = snprintf(line, sizeof line, "type=%s\n", type);
    }
    if (n > 0 && (size_t)n < sizeof line) write_all_fd(fd, line, (size_t)n);
    close(fd);
}

static int random_hex(char *out, size_t hexchars) {
    unsigned char raw[32];
    size_t need = (hexchars + 1) / 2;
    if (need > sizeof raw) return -1;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < need) {
        ssize_t n = read(fd, raw + got, need - got);
        if (n <= 0) { close(fd); return -1; }
        got += (size_t)n;
    }
    close(fd);
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < hexchars; i++)
        out[i] = hex[(raw[i / 2] >> (i % 2 ? 0 : 4)) & 0xF];
    out[hexchars] = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Persistence: config, token, TV state                                */
/* ------------------------------------------------------------------ */

static int atomic_write(const char *path, const char *data, size_t len, mode_t mode) {
    char tmp[640];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); unlink(tmp); return -1; }
        off += (size_t)n;
    }
    fsync(fd);
    close(fd);
    if (chmod(tmp, mode) || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}

static char *read_file(const char *path, size_t *len_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || st.st_size > (off_t)RESP_CAP) { close(fd); return NULL; }
    size_t sz = (size_t)st.st_size;
    char *buf = malloc(sz + 1);
    if (!buf) { close(fd); return NULL; }
    size_t got = 0;
    while (got < sz) {
        ssize_t n = read(fd, buf + got, sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    buf[got] = 0;
    if (len_out) *len_out = got;
    return buf;
}

#include "../../modules/jellyfin/config.inc"

/* TV browse checkpoint: one shared slot keyed "hr54", as in server.py. */
static void tv_state_path(char *path, size_t sz) {
    snprintf(path, sz, "%s/state/tv-state.json", persist_root);
}

static void tv_path_json(struct sb *out, struct jval *path) {
    sb_puts(out, "[");
    int comma = 0;
    if (path && path->t == J_ARR) for (size_t i = 0; i < path->n && i < 8; i++) {
        const char *id = jstr(jget(path->items[i], "id"));
        const char *name = jstr(jget(path->items[i], "name"));
        if (!id || !*id || strlen(id) > 70) continue;
        char label[256]; snprintf(label, sizeof label, "%.200s", name ? name : "");
        if (comma++) sb_puts(out, ",");
        sb_puts(out, "{\"id\":"); sb_json_str(out, id);
        sb_puts(out, ",\"name\":"); sb_json_str(out, label); sb_puts(out, "}");
    }
    sb_puts(out, "]");
}

/* Caller holds the state lock. */
static int tv_state_save_locked(struct jval *payload) {
    char path[512];
    tv_state_path(path, sizeof path);
    char lib[80] = "", submitted[80] = "", query[80] = "", zone[24] = "";
    int page = 0, index = 0, searching = 0;
    const char *s;
    if ((s = jstr(jget(payload, "lib")))) snprintf(lib, sizeof lib, "%.64s", s);
    if ((s = jstr(jget(payload, "submitted"))))
        snprintf(submitted, sizeof submitted, "%.64s", s);
    if ((s = jstr(jget(payload, "query")))) snprintf(query, sizeof query, "%.64s", s);
    if ((s = jstr(jget(payload, "zone")))) snprintf(zone, sizeof zone, "%.16s", s);
    if (!zone[0]) snprintf(zone, sizeof zone, "card");
    page = (int)jnum(jget(payload, "page"), 0);
    if (page < 0) page = 0;
    if (page > 100000) page = 100000;
    index = (int)jnum(jget(payload, "index"), 0);
    if (index < 0) index = 0;
    if (index > 5) index = 5;
    searching = jbool(jget(payload, "searching"), 0);
    struct sb b = {0};
    sb_puts(&b, "{\"hr54\":{\"lib\":");
    sb_json_str(&b, lib);
    sb_fmt(&b, ",\"page\":%d,\"searching\":%s,\"submitted\":", page,
           searching ? "true" : "false");
    sb_json_str(&b, submitted);
    sb_puts(&b, ",\"query\":");
    sb_json_str(&b, query);
    sb_puts(&b, ",\"zone\":");
    sb_json_str(&b, zone);
    sb_fmt(&b, ",\"index\":%d,\"path\":", index);
    tv_path_json(&b, jget(payload, "path"));
    sb_puts(&b, "}}\n");
    int rc = b.p ? atomic_write(path, b.p, b.len, 0600) : -1;
    free(b.p);
    return rc;
}

static int tv_state_load(struct sb *out) {
    char path[512];
    tv_state_path(path, sizeof path);
    size_t n = 0;
    char *raw = read_file(path, &n);
    if (!raw) { sb_puts(out, "{}"); return 0; }
    struct jval *v = json_parse(raw, n);
    free(raw);
    struct jval *slot = v ? jget(v, "hr54") : NULL;
    if (!slot || slot->t != J_OBJ) {
        jfree(v);
        sb_puts(out, "{}");
        return 0;
    }
    static const char *fields[] = {"lib", "page", "searching", "submitted",
                                   "query", "zone", "index", "path"};
    sb_puts(out, "{");
    for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) {
        struct jval *f = jget(slot, fields[i]);
        if (i) sb_puts(out, ",");
        sb_json_str(out, fields[i]);
        sb_puts(out, ":");
        if (!f) { sb_puts(out, "null"); continue; }
        switch (f->t) {
        case J_ARR: tv_path_json(out, f); break;
        case J_STR: sb_json_str(out, f->str); break;
        case J_NUM: sb_fmt(out, "%.0f", f->num); break;
        case J_TRUE: sb_puts(out, "true"); break;
        case J_FALSE: sb_puts(out, "false"); break;
        default: sb_puts(out, "null"); break;
        }
    }
    sb_puts(out, "}");
    jfree(v);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Jellyfin API calls                                                  */
/* ------------------------------------------------------------------ */

#include "../../modules/jellyfin/library.inc"

/* Playback state machine                                              */
/* ------------------------------------------------------------------ */

#include "hr54_iptv_library.h"
#include "hr54_iptv_clock.h"

static struct stream_slot *slot_by_token(const char *token) {
    for (int i = 0; i < JF_STREAM_SLOTS; i++)
        if (S->streams[i].in_use && !strcmp(S->streams[i].token, token))
            return &S->streams[i];
    return NULL;
}

static struct stream_slot *active_slot_locked(void) {
    return S->playing && S->play_token[0] ? slot_by_token(S->play_token) : NULL;
}

static void playback_end_locked(void) {
    if (S->play_session[0])
        snprintf(S->stop_pending, sizeof S->stop_pending, "%.63s", S->play_session);
    S->play_session[0] = 0;
    for (int i = 0; i < JF_STREAM_SLOTS; i++)
        if (S->streams[i].in_use) {
            S->streams[i].closed = 1;
            S->streams[i].in_use = 0;
        }
    if (S->return_to_tv) S->ui_return_source = S->play_live == 3 ? 4 : (S->play_live == 2 ? 3 : (S->play_live ? 2 : 1));
    S->yt_active = 0; S->yt_video[0]=S->yt_audio[0]=0;
    S->iptv_active = 0;
    S->iptv_worker = 0;
    S->play_live = 0;
    S->playing = 0;
    S->play_name[0] = 0;
    S->play_item[0] = 0;
    S->play_token[0] = 0;
    S->return_to_tv = 0;
    S->play_duration = 0;
    S->play_started = 0;
    S->play_base = 0;
}

/* The single commit point for every playback source. Returns the session
 * generation so committed-session responses can carry the same identity
 * the native shell later observes through /api/state. */
static unsigned long playback_begin_locked(const char *token, const char *name,
                                          const char *item_id, int base, int return_to_tv) {
    ++S->play_generation;
    jf_log_ms("playback session generation %lu committed (%s)", S->play_generation, name && *name ? name : "unnamed");
    for (int i = 0; i < JF_STREAM_SLOTS; i++) {
        struct stream_slot *s = &S->streams[i];
        if (s->in_use && strcmp(s->token, token)) {
            s->closed = 1;
            s->in_use = 0;
        }
    }
    snprintf(S->play_token, sizeof S->play_token, "%s", token);
    snprintf(S->play_name, sizeof S->play_name, "%.200s", name);
    snprintf(S->play_item, sizeof S->play_item, "%.70s", item_id);
    struct stream_slot *active = slot_by_token(token);
    if (active && active->session[0])
        snprintf(S->play_session, sizeof S->play_session, "%.63s", active->session);
    else
        S->play_session[0] = 0;
    S->playing = 1;
    S->play_duration = active ? active->duration : 0;
    S->play_live = 0; /* A prior Frigate session must not make Jellyfin live. */
    S->play_started = mono_now();
    S->play_base = base;
    S->return_to_tv = native_frontend ? 0 : return_to_tv;
    return S->play_generation;
}

static double playback_elapsed_locked(void) {
    struct stream_slot *s = active_slot_locked();
    if (!s) return S->play_live ? mono_now() - S->play_started : (double)S->play_base;
    double now = mono_now();
    double total = now - S->play_started - s->paused_total;
    if (s->paused) total -= now - s->paused_since;
    if (total < 0) total = 0;
    return (double)S->play_base + total;
}

static void status_common(struct sb *out) {
    sb_puts(out, ",\"receiver\":true,\"receiverHost\":");
    sb_json_str(out, S->lan_addr);
    sb_puts(out, ",\"user\":");
    sb_json_str(out, S->user_name);
}

/* GET /api/status */
static int playback_snapshot(struct sb *out) {
    state_lock();
    if (!S->playing) {
        sb_puts(out, "{\"playing\":false");
        status_common(out);
        sb_puts(out, "}");
        state_unlock();
        return 0;
    }
    struct stream_slot *s = active_slot_locked();
    char name[JF_NAME_MAX], item[JF_ID_MAX];
    snprintf(name, sizeof name, "%s", S->play_name);
    snprintf(item, sizeof item, "%s", S->play_item);
    int return_to_tv = S->return_to_tv;
    double elapsed = playback_elapsed_locked();
    int paused = s ? s->paused : 0;
    int live = S->play_live;
    double duration = live == 3 ? S->yt_duration : S->play_duration;
    int draining = s ? s->draining : 0;
    state_unlock();
    sb_puts(out, "{\"playing\":true,\"name\":");
    sb_json_str(out, name);
    sb_puts(out, ",\"itemId\":");
    sb_json_str(out, item);
    sb_fmt(out, ",\"returnToTv\":%s,\"elapsed\":%d,\"paused\":%s",
           return_to_tv ? "true" : "false", (int)elapsed, paused ? "true" : "false");
    sb_fmt(out, ",\"source\":\"%s\",\"live\":%s", live == 3 ? "youtube" : (live == 2 ? "iptv" : (live ? "frigate" : "jellyfin")), live && live!=3 ? "true" : "false");
    if (live == 2) { sb_puts(out, ",\"channelId\":"); sb_json_str(out,item); sb_puts(out, ",\"channelName\":"); sb_json_str(out,name); }
    sb_fmt(out, ",\"draining\":%s", draining ? "true" : "false");
    sb_puts(out, ",\"duration\":");
    if (duration > 0) sb_fmt(out, "%.0f", duration); else sb_puts(out, "null");
    status_common(out);
    sb_puts(out, "}");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Receiver control (on-box: uconntest, dt, SHEF, hr54-play-url)       */
/* ------------------------------------------------------------------ */

static int run_shell(char *out, size_t outsz, int timeout_s, const char *fmt, ...) {
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof cmd) return -1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    int fds[2];
    if (pipe(fds)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        signal(SIGCHLD, SIG_DFL);
        close(fds[0]);
        if (fds[1] != STDOUT_FILENO) dup2(fds[1], STDOUT_FILENO);
        if (fds[1] != STDERR_FILENO) dup2(fds[1], STDERR_FILENO);
        if (fds[1] != STDOUT_FILENO && fds[1] != STDERR_FILENO) close(fds[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    size_t got = 0;
    int status = -1, timed_out = 0;
    struct pollfd pf = {fds[0], POLLIN, 0};
    for (;;) {
        int pr = poll(&pf, 1, timeout_s * 1000);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) { timed_out = 1; break; }
        char buf[4096];
        ssize_t n2 = read(fds[0], buf, sizeof buf);
        if (n2 < 0) { if (errno == EINTR) continue; break; }
        if (n2 == 0) break;
        if (out && got + 1 < outsz) {
            size_t take = (size_t)n2;
            if (got + take + 1 > outsz) take = outsz - got - 1;
            memcpy(out + got, buf, take);
            got += take;
        }
    }
    if (timed_out) kill(pid, SIGKILL);
    close(fds[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (out && outsz) out[got] = 0;
    if (timed_out || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static int rc_key(const char *name) {
    char pathq[256];
    snprintf(pathq, sizeof pathq, "/remote/processKey?key=%s&hold=keyPress", name);
    struct jf_resp r;
    if (jf_http("127.0.0.1", JF_SHEF_PORT, "GET", pathq, NULL, NULL, NULL, &r, 4096))
        return fail("receiver SHEF key request failed");
    int ok = r.status == 200;
    resp_free(&r);
    return ok ? 0 : -1;
}

/* Native clients own transport keys. A SHEF STOP would be routed back to
 * that client rather than to the stock player. Keep this receiver operation
 * behind the API and use the existing DirectTest watch speed branch.
 * The compile-time path override exists only for isolated contract tests. */
#ifndef JF_UCONNECT_BIN
#define JF_UCONNECT_BIN "/opt/middleware_core/system/tv/uconntest"
#endif
static int receiver_stop(void) {
    if (!native_frontend) return rc_key("stop");
    char reply[4096] = {0};
    run_shell(reply, sizeof reply, 12,
        JF_UCONNECT_BIN " '<com.ucentric.pvruconnect.DirectTest command=\"watch\" session=\"0\" speed=\"stop\"/>' 2>&1");
    /* This vendor program's exit status is not a POSIX success status. */
    if (!strstr(reply, "pvruconnect.DirectTest success") ||
        strstr(reply, "Exception") || strstr(reply, "ERROR"))
        return fail("receiver stop failed");
    return 0;
}

static const char *DRUID_PROBE_XML =
    "<com.directv.druid.dt.DruidTester command=\"getCurrentScreenId\" "
    "session=\"local\"/>";

/* Screen id plus boot-OSD flag in one shell round trip. */
/* Druid can go briefly unresponsive while the middleware stack restarts, and
 * the watcher then probes several times a second.  Report it at most every
 * 10s so a sick stack cannot flood the log. */
static double probe_log_at = 0;
static int rc_probe(int *screen, int *boot_osd) {
    char out[4096];
    int rc = run_shell(out, sizeof out, 12,
        "osd=/var/mw_registry/Registry/Device/Server/OSD/Current; "
        "echo \"OSDNUM=$(cat $osd/Number 2>/dev/null)\"; "
        "/opt/middleware_core/system/tv/uconntest '%s' 2>&1", DRUID_PROBE_XML);
    *screen = -1;
    *boot_osd = 0;
    char *line = out;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = 0;
        if (!strncmp(line, "OSDNUM=", 7)) {
            char *v = line + 7;
            while (*v == ' ' || *v == '"') v++;
            *boot_osd = !strncmp(v, "36", 2) && (v[2] == 0 || v[2] == '"');
        }
        const char *marker = "Current screenId for session 0 is ";
        char *m = strstr(line, marker);
        if (m) *screen = atoi(m + strlen(marker));
        line = eol ? eol + 1 : NULL;
    }
    if (*screen < 0) {
        double t = mono_now();
        if (t - probe_log_at > 10.0) {
            probe_log_at = t;
            jf_log("screen probe lacked a screen id (helper rc %d): %.200s", rc, out);
        }
        return -1;
    }
    return 0;
}

#ifndef JF_BOOT_OSD_NUMBER
#define JF_BOOT_OSD_NUMBER "/var/mw_registry/Registry/Device/Server/OSD/Current/Number"
#endif
#ifndef JF_DT_BIN
#define JF_DT_BIN "/usr/bin/dt"
#endif
static int rc_clear_boot_osd(void) {
    return run_shell(NULL, 0, 15,
        "if [ \"$(cat " JF_BOOT_OSD_NUMBER " 2>/dev/null)\" = '\"36\"' ]; then "
        JF_DT_BIN " removeOsd -osd 36 -session 0 >/dev/null 2>&1; fi") ? -1 : 0;
}

/* A stock boot alert can retain a stale frame underneath a native surface.
 * Dismiss only that exact OSD through dt's supported removeOsd command.  This
 * path remains valid when the Druid context is deliberately not running and
 * never synthesizes navigation or impersonates input owners. */
static int native_boot_alert(void) {
    FILE *f = fopen(JF_BOOT_OSD_NUMBER, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    char value[64] = {0};
    int bad = !fgets(value, sizeof value, f) || ferror(f);
    fclose(f);
    if (bad) return -1;
    char *p = value;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '"') p++;
    return p[0] == '3' && p[1] == '6' &&
           (!p[2] || p[2] == '"' || isspace((unsigned char)p[2]));
}
#ifndef JF_DRUID_AUTO_START
#define JF_DRUID_AUTO_START "/var/mw_registry/Registry/Ucentric.CORE/Context/directv.DRUID/Auto-Start.str"
#endif
static int native_druid_disabled(void) {
    FILE *f=fopen(JF_DRUID_AUTO_START,"r");
    if(!f)return 0;
    char value[8]={0};size_t n=fread(value,1,sizeof(value),f);int bad=ferror(f);fclose(f);
    return !bad&&n==5&&!memcmp(value,"false",5);
}
#ifndef JF_NATIVE_WM_GUARD
#define JF_NATIVE_WM_GUARD "md5sum /opt/dtvwm/lib/libdtvwm.so /opt/dtvwm/bin/dtvwm"
#endif
#ifndef JF_NATIVE_WM_PORT
#define JF_NATIVE_WM_PORT 2017
#endif
/* Native-exclusive must also dismiss DTVWM's independent stock I-frame.
 * Auto-Start=false removes Druid, NOT Session::IFrameConfiguration. On this
 * exact firmware the checking-satellite image remains in Session's EGL shim.
 * Command40 is [blender, desiredBits, CHANGE mask], not enable/disable masks.
 * Session::enablePlanes 0x3f8fc..0x3f948 maps ONLY bit1 to I-frame visibility;
 * bits2/4/8 are video/graphics/PIP and must not be touched. getEnabledPlanes
 * omits bit1, so it can verify preservation, not I-frame visibility itself.
 * No bitmap allocation, guessed stock bitmap ID, or shared-memory writes. */
static int native_wm_transfer(int fd, void *data, size_t size, int receive) {
    unsigned char *p=data;
    while(size){
        ssize_t n=receive?recv(fd,p,size,0):send(fd,p,size,MSG_NOSIGNAL);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return -1;
        p+=n;size-=(size_t)n;
    }
    return 0;
}
static int native_wm_request(int fd, unsigned command, const unsigned *data,
                             size_t count, unsigned *value) {
    uint32_t packet[7]={htonl(command),htonl(0xd123567a),htonl((unsigned)count*4),0};
    uint32_t reply[16];
    if(count>3)return -1;
    for(size_t i=0;i<count;i++)packet[4+i]=htonl(data[i]);
    if(native_wm_transfer(fd,packet,16+count*4,0)||
       native_wm_transfer(fd,reply,sizeof(reply),1))return -1;
    unsigned type=ntohl(reply[0]),result=ntohl(reply[1]);
    if(result||type!=(command==42?3u:2u))return -1;
    if(value)*value=ntohl(reply[2]);
    return 0;
}
static int native_clear_stock_iframe(void) {
    if(!native_frontend||!native_druid_disabled())return 0;
    char hashes[256]={0};
    if(run_shell(hashes,sizeof hashes,5,JF_NATIVE_WM_GUARD)||
       !strstr(hashes,"0222999c41c9a57dabd8c9b3d714b69e")||
       !strstr(hashes,"fed62d04663412e60388ac09a83cbc05"))
        return fail("unsupported native window-manager firmware");
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0)return fail("native presentation connection failed");
    fcntl(fd,F_SETFD,FD_CLOEXEC);
    struct timeval timeout={3,0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(JF_NATIVE_WM_PORT)};
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    uint32_t setup[16]={htonl(1),0,htonl((unsigned)getpid())};
    unsigned data[3]={0,0,1},before=0,after=0;
    int bad=connect(fd,(struct sockaddr *)&address,sizeof address)||
        native_wm_transfer(fd,setup,sizeof setup,0)||
        native_wm_request(fd,42,data,2,&before)||
        native_wm_request(fd,40,data,3,NULL)||
        native_wm_request(fd,42,data,2,&after);
    close(fd);
    if(bad||before!=after)return fail("native stock-frame dismissal failed");
    jf_log("native presentation: stock I-frame disabled; video/graphics mask unchanged 0x%x",after);
    return 0;
}
static int native_frontend_prepare(struct sb *out) {
    /* OSD/Current and DTVWM's still-image producer survive Druid suppression.
     * The dead Druid removeOsd adapter cannot dismiss the latter. */
    if(native_druid_disabled()){
        if(native_clear_stock_iframe())return -1;
        sb_puts(out,"{\"prepared\":true,\"dismissedBootAlert\":false,\"stockContextDisabled\":true}");
        return 0;
    }
    int alert = native_boot_alert();
    if (alert < 0) return fail("receiver alert state unavailable");
    if (alert) {
        if (rc_clear_boot_osd()) return fail("receiver boot alert dismissal failed");
        double end = mono_now() + 2.0;
        while ((alert = native_boot_alert()) == 1 && mono_now() < end) nap(0.05);
        if (alert != 0) return fail("receiver boot alert still active");
        jf_log("native frontend: stock boot alert dismissed before input acquisition");
        sb_puts(out, "{\"prepared\":true,\"dismissedBootAlert\":true}");
    } else sb_puts(out, "{\"prepared\":true,\"dismissedBootAlert\":false}");
    return 0;
}

/* Druid refuses itvStartApp ("StartAppTask not starting ... as UI loading")
 * until presentation is back on LiveTV, so the screen state is polled after
 * every EXIT rather than waited out with a fixed sleep. */
static int rc_walk_to_live_tv(int screen) {
    double started = mono_now();
    int steps = 0;
    for (;;) {
        if (screen < 0) {
            int boot_osd;
            if (rc_probe(&screen, &boot_osd)) return -1;
        }
        if (screen == LIVE_TV_SCREEN) {
            jf_log_ms("Druid at LiveTV 2320 after %d EXIT step(s), %.3fs",
                      steps, mono_now() - started);
            return 0;
        }
        if (++steps > WALK_MAX_STEPS ||
            mono_now() - started > WALK_BUDGET) {
            jf_log_ms("walk to LiveTV gave up on screen %d after %d step(s), "
                      "%.3fs", screen, steps, mono_now() - started);
            return -1;
        }
        jf_log_ms("EXIT for screen %d (step %d)", screen, steps);
        if (rc_key("exit")) return -1;
        nap(WALK_SETTLE);
        screen = -1;
    }
}

static int rc_prepare_itv(void) {
    return run_shell(NULL, 0, 20,
        "if ! pgrep -f '/opt/itv/itvpack/root/bin/[i]tv -i 0' >/dev/null; then "
        "/opt/itv/itvpack/root/bin/start.sh -i 0 >/tmp/jellyfin-itv.log 2>&1 & "
        "sleep 12; fi") ? -1 : 0;
}

static int rc_launch_itv(const char *url) {
    char out[4096];
    int rc = run_shell(out, sizeof out, 15,
        "/opt/middleware_core/system/tv/uconntest "
        "'<com.ucentric.pvruconnect.DirectTest command=\"itvStartApp\" "
        "sessionId=\"0\" url=\"%s\"/>' 2>&1", url);
    /* uconntest uses vendor-specific nonzero success statuses.  Its response
     * body, not the process status, is authoritative. */
    if (strstr(out, "<Error>") || (!out[0] && rc)) return -1;
    return 0;
}

static int rc_itv_running(void) {
    char out[4096];
    int rc = run_shell(out, sizeof out, 12,
        "/opt/middleware_core/system/tv/uconntest "
        "'<com.ucentric.pvruconnect.DirectTest command=\"itvGetAppStatus\" "
        "sessionId=\"0\"/>' 2>&1");
    if (strstr(out, "Running appId")) return 1;
    if (strstr(out, "No app running")) return 0;
    jf_log("ITV status response unrecognized (helper rc %d): %.200s", rc, out);
    return -1;
}

static void tv_url(char *out, size_t sz) {
    snprintf(out, sz, "http://%s:%d/", S->lan_addr, S->listen_port);
}

/* Shared tail of every TV presentation: build the URL, start ITV, then
 * discard launcher-generated EXIT presses before the new page polls. */
static void rc_launch_tv_ui(void) {
    char url[128];
    tv_url(url, sizeof url);
    state_lock();
    int source = S->ui_return_source;
    S->ui_return_source = 0;
    state_unlock();
    if (source) strncat(url, source == 4 ? "?source=youtube" : (source == 3 ? "?source=iptv" : (source == 2 ? "?source=frigate" : "?source=jellyfin")), sizeof url - strlen(url) - 1);
    jf_log_ms("itvStartApp issuing for %s", url);
    if (rc_launch_itv(url)) jf_log("ITV launch failed");
    else jf_log_ms("itvStartApp accepted");
    struct stat keys;
    state_lock();
    S->ui_key_until = mono_now();
    if (!stat("/var/viewer/scan.log", &keys)) {
        S->ui_key_offset = keys.st_size; S->ui_key_inode = keys.st_ino;
    }
    state_unlock();
}

/* Clear boot OSD, walk Druid to LiveTV, then start the app: all three are
 * required for a clean full-screen view. */
static void rc_present_tv_ui(int screen) {
    if (native_frontend) return;
    rc_clear_boot_osd();
    rc_walk_to_live_tv(screen);
    rc_launch_tv_ui();
}

/* MENU takeover.  Identical sequence to rc_present_tv_ui, except the ITV
 * process start is moved behind the walk: start.sh waits on its own startup,
 * and doing that first left the stock menu sitting onscreen for the whole
 * wait even though nothing about it needed the app to be running. */
static void rc_takeover_from_menu(int screen, int boot_osd) {
    if (native_frontend) return;
    jf_log_ms("menu takeover begins from screen %d (OSD 36 %s)", screen,
              boot_osd ? "present" : "not present");
    if (rc_clear_boot_osd()) jf_log("OSD 36 clear reported failure");
    else jf_log_ms("OSD 36 remove issued");
    rc_walk_to_live_tv(screen);
    if (rc_prepare_itv()) jf_log("ITV preparation failed");
    rc_launch_tv_ui();
}

/* Post-stop UI return, ported from server.py's schedule_tv_return. */
static void schedule_tv_return(int return_to_tv, double delay) {
    if (native_frontend) return; /* Native clients observe /api/state. */
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_IGN);
        alarm(0);
        struct timespec ts = {(time_t)delay, 0};
        nanosleep(&ts, NULL);
        int playing = 0;
        state_lock();
        playing = S->playing;
        state_unlock();
        if (playing) _exit(0);
        if (return_to_tv) rc_present_tv_ui(-1);
        else rc_key("menu");
        _exit(0);
    }
}

/* ------------------------------------------------------------------ */
/* /api/play: PlaybackInfo -> opaque local URL -> on-box playURL       */
/* ------------------------------------------------------------------ */

static void play_url_for(const char *token, char *out, size_t sz) {
    const char *host = S->loopback_failed ? S->lan_addr : "127.0.0.1";
    snprintf(out, sz, "http://%s:%d/play/%s.ts", host, S->listen_port, token);
}

static int invoke_play_url(const char *url) {
    if(native_clear_stock_iframe())return -1;
    int rc = run_shell(NULL, 0, 60, "%s%s '%s'", native_frontend ? "HR54_NATIVE_FRONTEND=1 " : "", play_cmd, url);
    if (rc) {
        jf_log("receiver refused playURL");
        return fail("receiver refused playURL");
    }
    return 0;
}

/* Keep the legacy scan-log boundary for non-native clients. Native input
 * is delivered by the broker; native playURL performs no key choreography. */
static void playback_setup_complete(void) {
    struct stat keys;
    state_lock();
    if (!stat("/var/viewer/scan.log", &keys)) {
        S->setup_key_offset = keys.st_size; S->setup_key_inode = keys.st_ino;
        S->ui_key_offset = keys.st_size; S->ui_key_inode = keys.st_ino;
    }
    S->ui_key_until = mono_now();
    state_unlock();
}

static int wait_for_claim(const char *token, double seconds) {
    for (double t = 0; t < seconds; t += 0.2) {
        int claimed = 0, closed = 0;
        state_lock();
        struct stream_slot *s = slot_by_token(token);
        claimed = s && s->ready;
        closed = !s || s->closed;
        state_unlock();
        if (closed) return -1;
        if (claimed) return 1;
        nap(0.2);
    }
    return 0;
}

/* Emits {"playing":true,"name":X,"itemId":Y[,"startSeconds":N] without the
 * closing brace; the caller closes the object (and may add extras). */
static int start_play(const char *item_id, double start_seconds, int return_to_tv,
                      struct sb *out) {
    state_lock();int iptv_busy=S->iptv_active||S->yt_active;state_unlock();
    if(iptv_busy)return fail("Stop IPTV before starting Jellyfin playback");
    char name[JF_NAME_MAX] = "", upath[JF_URL_MAX], token[64], url[512];
    char session[64];
    double duration = 0;
    if (transcode_url(item_id, start_seconds, name, sizeof name, upath, sizeof upath,
                      session, sizeof session, &duration))
        return -1;
    state_lock();
    playback_end_locked();
    state_unlock();
    flush_stop_report();
    for (int attempt = 0; attempt < 2; attempt++) {
        if (random_hex(token, 24)) return fail("cannot read /dev/urandom");
        state_lock();
        struct stream_slot *slot = NULL;
        for (int i = 0; i < JF_STREAM_SLOTS; i++)
            if (!S->streams[i].in_use) { slot = &S->streams[i]; break; }
        if (!slot) {
            state_unlock();
            return fail("too many pending streams");
        }
        memset(slot, 0, sizeof *slot);
        slot->in_use = 1;
        slot->duration = duration;
        snprintf(slot->token, sizeof slot->token, "%s", token);
        snprintf(slot->upath, sizeof slot->upath, "%s", upath);
        snprintf(slot->session, sizeof slot->session, "%.63s", session);
        state_unlock();
        play_url_for(token, url, sizeof url);
        state_lock(); S->ui_key_until = mono_now() + 6; state_unlock();
        if (invoke_play_url(url)) {
            state_lock();
            struct stream_slot *s = slot_by_token(token);
            if (s) { s->closed = 1; s->in_use = 0; }
            state_unlock();
            return -1;
        }
        playback_setup_complete();
        int claim = wait_for_claim(token, CLAIM_WAIT);
        if (claim < 0) {
            state_lock();
            struct stream_slot *s = slot_by_token(token);
            if (s) { s->closed = 1; s->in_use = 0; }
            state_unlock();
            schedule_tv_return(return_to_tv, POST_STOP_DISMISS);
            return fail("Jellyfin stream failed before playback became ready");
        }
        if (claim > 0) {
            state_lock();
            struct stream_slot *s = slot_by_token(token);
            if (!s || s->closed) {
                if (s) s->in_use = 0;
                state_unlock();
                schedule_tv_return(return_to_tv, POST_STOP_DISMISS);
                return fail("Jellyfin stream ended before playback became ready");
            }
            unsigned long gen = playback_begin_locked(token, name, item_id, (int)start_seconds, return_to_tv);
            state_unlock();
            jf_log("now playing: %s (generation %lu)", name, gen);
            sb_puts(out, "{\"playing\":true,\"name\":");
            sb_json_str(out, name);
            sb_puts(out, ",\"itemId\":");
            sb_json_str(out, item_id);
            sb_fmt(out, ",\"generation\":%lu", gen);
            if (start_seconds > 0)
                sb_fmt(out, ",\"startSeconds\":%d", (int)start_seconds);
            return 0;
        }
        /* The decoder never fetched the stream: if the URL was loopback,
         * fall back to the receiver's LAN address exactly once. */
        state_lock();
        struct stream_slot *s = slot_by_token(token);
        if (s) { s->closed = 1; s->in_use = 0; }
        int was_loopback = !S->loopback_failed;
        S->loopback_failed = 1;
        state_unlock();
        if (!was_loopback) break;
        jf_log("playURL did not open the loopback stream; retrying on LAN address");
    }
    schedule_tv_return(return_to_tv, POST_STOP_DISMISS);
    return fail("receiver did not open a ready Jellyfin stream");
}


/* Frigate integration: only names/codecs leave the backend. Config may
 * contain camera credentials; never forward its body to the TV or logs. */
#ifndef FRIGATE_HOST
#define FRIGATE_HOST "192.168.88.39"
#endif
#ifndef FRIGATE_PORT
#define FRIGATE_PORT 5000
#endif
static struct jval *frigate_get(const char *path) {
    struct jf_resp r = {0};
    if (jf_http(FRIGATE_HOST, FRIGATE_PORT, "GET", path, NULL, NULL, NULL, &r, RESP_CAP)) {
        fail("Frigate connection failed"); return NULL;
    }
    struct jval *v = r.status == 200 ? json_parse(r.body, r.body_len) : NULL;
    if (!v) fail("Frigate API unavailable (HTTP %d)", r.status);
    free(r.body);
    return v;
}
static int frigate_h264(const char *stream) {
    struct sb path = {0};
    sb_puts(&path, "/api/go2rtc/streams/"); url_encode(&path, stream);
    struct jval *v = frigate_get(path.p); free(path.p);
    struct jval *producers = jget(v, "producers");
    int h264 = 0, other = 0;
    if (producers && producers->t == J_ARR)
        for (size_t i = 0; i < producers->n; i++) {
            struct jval *medias = jget(producers->items[i], "medias");
            if (medias && medias->t == J_ARR)
                for (size_t j = 0; j < medias->n; j++) {
                    const char *media = jstr(medias->items[j]);
                    if (!media || strncmp(media, "video,", 6)) continue;
                    if (strstr(media, "H264")) h264 = 1; else other = 1;
                }
        }
    jfree(v);
    /* MPEG-TS in go2rtc 1.9.9 ignores codec query filters. Do not select
       a stream with competing HEVC producers, even if H264 also appears. */
    return h264 && !other;
}
static int frigate_camera(struct jval *config, const char *id,
                          char *stream, size_t sz) {
    struct jval *c = jget(jget(config, "cameras"), id);
    if (!c || !jbool(jget(c, "enabled"), 1)) return fail("Camera is disabled or absent");
    struct jval *live = jget(jget(c, "live"), "streams");
    if (live && live->t == J_OBJ)
        for (size_t i = 0; i < live->n; i++) {
            const char *name = jstr(live->items[i]);
            if (name && strlen(name) < sz && frigate_h264(name)) {
                snprintf(stream, sz, "%s", name); return 0;
            }
        }
    /* Try only configured conventional substreams, never camera URLs. */
    char sub[256]; snprintf(sub, sizeof sub, "%s_sub", id);
    struct jval *streams = jget(jget(config, "go2rtc"), "streams");
    if (jget(streams, sub) && strlen(sub) < sz && frigate_h264(sub)) {
        snprintf(stream, sz, "%s", sub); return 0;
    }
    int tried_id = 0;
    if (live && live->t == J_OBJ) for (size_t i = 0; i < live->n; i++) {
        const char *name = jstr(live->items[i]);
        if (name && !strcmp(name, id)) tried_id = 1;
    }
    if (!tried_id && jget(streams, id) && strlen(id) < sz && frigate_h264(id)) {
        snprintf(stream, sz, "%s", id); return 0;
    }
    return fail("No reachable H.264 restream for this camera");
}
static int frigate_cameras(struct sb *out) {
    struct jval *config = frigate_get("/api/config");
    if (!config) return -1;
    struct jval *cams = jget(config, "cameras");
    sb_puts(out, "{\"cameras\":[");
    if (cams && cams->t == J_OBJ)
        for (size_t i = 0; i < cams->n; i++) {
            char stream[256] = "";
            int compatible = frigate_camera(config, cams->keys[i], stream, sizeof stream) == 0;
            if (i) sb_puts(out, ",");
            sb_puts(out, "{\"id\":"); sb_json_str(out, cams->keys[i]);
            sb_puts(out, ",\"name\":"); sb_json_str(out, cams->keys[i]);
            sb_puts(out, ",\"stream\":"); sb_json_str(out, stream);
            sb_fmt(out, ",\"playable\":%s,\"live\":true,\"reason\":", compatible ? "true" : "false");
            sb_json_str(out, compatible ? "" : "No reachable H.264 restream");
            sb_puts(out, "}");
        }
    sb_puts(out, "]}"); jfree(config); return 0;
}
static int frigate_play(const char *camera, struct sb *out) {
    if (!camera || !*camera) return fail("cameraId required");
    state_lock();int iptv_busy=S->iptv_active||S->yt_active;state_unlock();
    if(iptv_busy)return fail("Stop IPTV before starting camera playback");
    struct jval *config = frigate_get("/api/config");
    if (!config) return -1;
    char stream[256] = "";
    int rc = frigate_camera(config, camera, stream, sizeof stream);
    jfree(config); if (rc) return -1;
    struct sb url = {0};
    /* Frigate 0.18 proxies go2rtc's MPEG-TS endpoint at the web root.
       The older /api/go2rtc/api/stream.ts route now returns 404. */
    sb_fmt(&url, "http://%s:%d/stream.ts?src=", FRIGATE_HOST, FRIGATE_PORT);
    url_encode(&url, stream);
    /* No relay/transcode: stock player fetches Frigate's HTTP TS directly. */
    state_lock(); S->ui_key_until = mono_now() + 6; state_unlock();
    if (invoke_play_url(url.p)) { free(url.p); return -1; }
    free(url.p);
    playback_setup_complete();
    state_lock();
    playback_end_locked();
    unsigned long gen = playback_begin_locked("", camera, camera, 0, 1);
    S->play_live = 1;
    S->ui_return_source = 0;
    state_unlock();
    jf_log("Frigate live camera %s stream %s (direct HTTP TS, generation %lu)", camera, stream, gen);
    sb_puts(out, "{\"playing\":true,\"source\":\"frigate\",\"live\":true,\"cameraId\":");
    sb_json_str(out, camera);
    sb_fmt(out, ",\"generation\":%lu}", gen);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Transport                                                           */
/* ------------------------------------------------------------------ */

static int transport_action(const char *action, struct sb *out) {
    state_lock();
    struct stream_slot *s = active_slot_locked();
    if (S->playing && S->play_live) {
        if (strcmp(action, "stop")) {
            state_unlock(); return fail("pause/resume unsupported for this source");
        }
        int return_to_tv = S->return_to_tv;
        playback_end_locked(); state_unlock();
        if (receiver_stop()) return -1;
        schedule_tv_return(return_to_tv, POST_STOP_DISMISS);
        sb_puts(out, "{\"stopped\":true}"); return 0;
    }
    if (!s) {
        state_unlock();
        return fail("nothing is playing");
    }
    if (s->draining && strcmp(action, "stop")) {
        state_unlock(); return fail("pause/resume unavailable while decoder drains; seek or stop instead");
    }
    if (!strcmp(action, "pause")) {
        int was = s->paused;
        if (!was) {
            s->paused = 1;
            s->paused_since = mono_now();
        }
        state_unlock();
        sb_puts(out, "{\"paused\":");
        sb_puts(out, "true");
        sb_puts(out, "}");
        jf_log("pause");
        return 0;
    }
    if (!strcmp(action, "resume")) {
        if (s->paused && mono_now() - s->paused_since > RESUME_RESTART) {
            char item[JF_ID_MAX];
            snprintf(item, sizeof item, "%s", S->play_item);
            double elapsed = playback_elapsed_locked();
            int return_to_tv = S->return_to_tv;
            playback_end_locked();
            state_unlock();
            jf_log("long pause: restarting stream");
            if (start_play(item, elapsed, return_to_tv, out)) return -1;
            sb_puts(out, ",\"resumed\":true}");
            return 0;
        }
        int was = s->paused;
        if (was) {
            s->paused_total += mono_now() - s->paused_since;
            s->paused = 0;
        }
        state_unlock();
        /* server.py: {"paused": not gate.resume()} */
        sb_puts(out, "{\"paused\":");
        sb_puts(out, "false");
        sb_puts(out, "}");
        jf_log("resume");
        return 0;
    }
    if (!strcmp(action, "stop")) {
        int return_to_tv = S->return_to_tv;
        playback_end_locked();
        state_unlock();
        flush_stop_report();
        jf_log("stop");
        if (native_frontend && receiver_stop()) return -1;
        schedule_tv_return(return_to_tv, POST_STOP_DISMISS);
        sb_puts(out, "{\"stopped\":true}");
        return 0;
    }
    state_unlock();
    return fail("unsupported transport action");
}

/* ------------------------------------------------------------------ */
/* HTTP responses                                                     */
/* ------------------------------------------------------------------ */

static void send_body(int fd, int code, const char *reason, const char *ctype,
                      const char *cache, const char *body, size_t len,
                      const char *method) {
    char head[640];
    int n = snprintf(head, sizeof head,
        "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s%s"
        "Connection: close\r\n\r\n",
        code, reason, ctype, len,
        cache ? "Cache-Control: " : "", cache ? cache : "");
    if (n < 0 || (size_t)n >= sizeof head) return;
    if (write_all_fd(fd, head, (size_t)n)) return;
    if (len && strcmp(method, "HEAD")) write_all_fd(fd, body, len);
}

static const char *http_reason(int code) {
    switch (code) {
    case 200: return "OK";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Body Too Large";
    case 431: return "Headers Too Large";
    case 500: return "Internal Server Error";
    default: return "Bad Gateway";
    }
}

static void send_json_sb(int fd, int code, const char *reason, struct sb *b,
                         const char *method) {
    /* Additive envelope preserves every field consumed by the TV client. */
    struct sb wrapped = {0};
    size_t json_len = b->len;
    while (json_len && isspace((unsigned char)b->p[json_len - 1])) json_len--;
    if (code < 400 && b->p && json_len >= 2 && b->p[0] == '{' &&
        !strstr(b->p, "\"ok\":")) {
        sb_puts(&wrapped, "{\"ok\":true");
        if (json_len > 2) { sb_puts(&wrapped, ","); sb_putn(&wrapped, b->p + 1, json_len - 2); }
        sb_puts(&wrapped, "}");
    }
    send_body(fd, code, reason, "application/json", NULL,
              wrapped.p ? wrapped.p : (b->p ? b->p : "{}"),
              wrapped.p ? wrapped.len : (b->p ? b->len : 2), method);
    free(wrapped.p);
}

static void send_json_error(int fd, int code, const char *msg, const char *method) {
    struct sb b = {0};
    sb_puts(&b, "{\"ok\":false,\"error\":");
    sb_json_str(&b, msg);
    sb_puts(&b, "}");
    send_json_sb(fd, code, http_reason(code), &b, method);
    free(b.p);
}

/* ------------------------------------------------------------------ */
/* Artwork                                                            */
/* ------------------------------------------------------------------ */

static int is_alnum_str(const char *s) {
    if (!s || !*s) return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s)) return 0;
    return 1;
}

static void serve_art(int fd, const char *method, const char *raw, int native_size) {
    char item_id[JF_ID_MAX];
    snprintf(item_id, sizeof item_id, "%s", raw);
    char *dot = strchr(item_id, '.');
    if (dot) *dot = 0;
    if (!is_alnum_str(item_id)) {
        send_json_error(fd, 404, "not found", method);
        return;
    }
    char data_path[600], ct_path[640];
    snprintf(data_path, sizeof data_path, "%s/cache/%s%s", persist_root, item_id, native_size ? "-native" : "");
    snprintf(ct_path, sizeof ct_path, "%s/cache/%s%s.ct", persist_root, item_id, native_size ? "-native" : "");
    size_t n = 0;
    char *body = read_file(data_path, &n);
    if (body) {
        char ct[128] = "image/jpeg";
        char *ctraw = read_file(ct_path, NULL);
        if (ctraw) {
            snprintf(ct, sizeof ct, "%.120s", ctraw);
            char *nl = strpbrk(ct, "\r\n");
            if (nl) *nl = 0;
            free(ctraw);
        }
        send_body(fd, 200, "OK", ct, "max-age=3600", body, n, method);
        free(body);
        return;
    }
    if (require_token()) {
        send_json_error(fd, 404, "no artwork", method);
        return;
    }
    struct sb path = {0};
    sb_fmt(&path, "Items/%s/Images/Primary", item_id);
    if (native_size) sb_puts(&path, "?maxWidth=512&maxHeight=512&quality=85");
    struct jf_resp r;
    if (jf_get(path.p, &r, RESP_CAP)) {
        free(path.p);
        send_json_error(fd, 404, "no artwork", method);
        return;
    }
    free(path.p);
    if (r.status != 200 || !r.body || !r.body_len) {
        resp_free(&r);
        send_json_error(fd, 404, "no artwork", method);
        return;
    }
    const char *ct = r.ctype[0] ? r.ctype : "image/jpeg";
    atomic_write(data_path, r.body, r.body_len, 0600);
    atomic_write(ct_path, ct, strlen(ct), 0600);
    send_body(fd, 200, "OK", ct, "max-age=3600", r.body, r.body_len, method);
    resp_free(&r);
}

#include "hr54_iptv_relay.h"
#include "hr54_youtube.h"

/* ------------------------------------------------------------------ */
/* /play/<opaque>.ts relay                                            */
/* ------------------------------------------------------------------ */

/* A decoder request alone does not establish healthy playback. Clear only
 * the session owned by this relay; an old relay must not end its successor. */
#include "../../modules/jellyfin/relay.inc"

/* Static TV files                                                    */
/* ------------------------------------------------------------------ */

static void serve_static(int fd, const char *method, const char *path) {
    const char *name = NULL, *type = NULL;
    if (!strcmp(path, "/") || !strcmp(path, "/index.html") ||
        !strcmp(path, "/tv") || !strcmp(path, "/tv/") ||
        !strcmp(path, "/tv/index.html")) {
        name = "tv/index.html"; type = "text/html; charset=utf-8";
    } else if (!strcmp(path, "/tv/app.js")) {
        name = "tv/app.js"; type = "application/javascript";
    } else if (!strcmp(path, "/tv/app.css")) {
        name = "tv/app.css"; type = "text/css";
    } else {
        send_json_error(fd, 404, "not found", method);
        return;
    }
    char target[1024];
    if (snprintf(target, sizeof target, "%s/%s", docroot, name) >= (int)sizeof target) {
        send_json_error(fd, 500, "path too long", method);
        return;
    }
    int source = open(target, O_RDONLY | O_NOFOLLOW);
    struct stat stt;
    if (source < 0 || fstat(source, &stt) || !S_ISREG(stt.st_mode)) {
        if (source >= 0) close(source);
        send_json_error(fd, 404, "not found", method);
        return;
    }
    char head[512];
    int n = snprintf(head, sizeof head,
        "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        type, (long long)stt.st_size);
    if (n < 0 || (size_t)n >= sizeof head) { close(source); return; }
    if (write_all_fd(fd, head, (size_t)n)) { close(source); return; }
    if (strcmp(method, "HEAD")) {
        char buffer[8192];
        ssize_t r;
        while ((r = read(source, buffer, sizeof buffer)) > 0)
            if (write_all_fd(fd, buffer, (size_t)r)) break;
    }
    close(source);
}

/* ------------------------------------------------------------------ */
/* API routing                                                        */
/* ------------------------------------------------------------------ */

/* EXIT does not reach the DOM on this firmware. The active TV polls this
 * small tail independently of the slower MENU/Druid watcher. */
static int tv_input(struct sb *out) {
    int fd = open("/var/viewer/scan.log", O_RDONLY);
    struct stat st;
    state_lock();
    if (fd >= 0 && !fstat(fd, &st)) {
        if (S->ui_key_inode != st.st_ino || S->ui_key_offset > st.st_size) {
            S->ui_key_inode = st.st_ino; S->ui_key_offset = st.st_size;
        }
        char buf[8192];
        ssize_t n = pread(fd, buf, sizeof buf - 1, S->ui_key_offset);
        if (n > 0) {
            buf[n] = 0;
            /* Keep a partial last log line for the next poll. */
            char *last = strrchr(buf, '\n');
            if (last) {
                S->ui_key_offset += last - buf + 1;
                last[1] = 0;
                if (!S->playing && mono_now() >= S->ui_key_until) {
                    char *p = buf;
                    while ((p = strstr(p, "rawkey=1e502"))) { S->ui_back_seq++; p += 12; }
                }
            }
        }
    }
    sb_fmt(out, "{\"back\":%u}", S->ui_back_seq);
    state_unlock(); if (fd >= 0) close(fd); return 0;
}

/* Defined further down, in the DOOM section. */
static int doom_armed(void);
static int doom_running_pid(void);
static int native_doom_busy(void);
static void doom_stop(const char *why);
static void doom_input_write(unsigned mask);
static unsigned doom_input_read(void);
static void api_doom_status(int fd);
static int native_doom_start(struct sb *out);

/* Lifecycle where practical: "playing" once a session is committed,
 * "preparing" while a start is claimed but not yet committed, "idle" after
 * cleanup. Derived from existing state; no new lifecycle source of truth. */
static void media_phase_locked(struct sb *out) {
    int pending = S->iptv_active || S->yt_active;
    if (!pending)
        for (int i = 0; i < JF_STREAM_SLOTS && !pending; i++)
            pending = S->streams[i].in_use && !S->streams[i].closed;
    sb_puts(out, S->playing ? "\"playing\"" : pending ? "\"preparing\"" : "\"idle\"");
}

/* Shared state, with no DOM/checkpoint dependence and no upstream probing. */
static void api_state(struct sb *out) {
    playback_snapshot(out);
    struct jval *snapshot = json_parse(out->p, out->len);
    int playing = jbool(jget(snapshot, "playing"), 0);
    const char *source = jstr(jget(snapshot, "source"));
    int transport = playing && source && !strcmp(source, "jellyfin");
    int can_gate = transport && !jbool(jget(snapshot, "draining"), 0);
    if (out->len) out->p[--out->len] = 0;
    sb_puts(out, ",\"title\":"); sb_json_str(out, jstr(jget(snapshot, "name")) ? jstr(jget(snapshot, "name")) : "");
    if (!playing) sb_puts(out, ",\"source\":null,\"itemId\":null,\"paused\":false,\"live\":false,\"elapsed\":0,\"duration\":null");
    sb_fmt(out, ",\"transport\":{\"stop\":%s,\"pause\":%s,\"resume\":%s,\"seek\":%s}",
        playing ? "true" : "false", can_gate ? "true" : "false",
        can_gate ? "true" : "false", transport ? "true" : "false");
    jfree(snapshot);
    state_lock();
    sb_fmt(out, ",\"jellyfinAuthenticated\":%s", S->auth_valid ? "true" : "false");
    sb_puts(out, ",\"phase\":");
    media_phase_locked(out);
    sb_fmt(out, ",\"generation\":%lu", S->play_generation);
    state_unlock();
    sb_puts(out, "}");
}
static void serve_doom_frame(int fd, const char *method, const char *query);

static void handle_api(int fd, const char *method, const char *route,
                       const char *query, const char *body) {
    struct jval *payload = json_parse(body && *body ? body : "{}",
                                      body && *body ? strlen(body) : 2);
    if (!payload || payload->t != J_OBJ) {
        jfree(payload);
        send_json_error(fd, 400, "bad json", method);
        return;
    }
    struct sb out = {0};
    int rc = 0;

    if (!strcmp(method, "GET") || !strcmp(method, "HEAD")) {
        if (!strcmp(route, "/api/capabilities")) {
            sb_fmt(&out, "{\"jellyfin\":true,\"iptv\":true,\"youtube\":true,\"frigate\":true,\"doom\":%s,\"playback\":true,\"settings\":true,\"settingsWritable\":true}", native_frontend ? "true" : "false");
        } else if (!strcmp(route, "/api/system/status")) {
            int doom_busy = native_frontend && native_doom_busy();
            state_lock(); int media_busy = S->playing || S->iptv_active || S->yt_active || doom_busy; state_unlock();
            sb_fmt(&out, "{\"ready\":true,\"frontend\":\"%s\",\"doomRunning\":%s,\"mediaBusy\":%s}", native_frontend ? "native" : "legacy", doom_busy ? "true" : "false", media_busy ? "true" : "false");
        } else if (!strcmp(route, "/api/state")) {
            api_state(&out);
        } else if (!strcmp(route, "/api/settings") || !strcmp(route, "/api/jellyfin/status")) {
            state_lock();
            jf_settings_locked(&out);
            state_unlock();
        } else if (!strcmp(route, "/api/frigate/status")) {
            rc = frigate_cameras(&out);
        } else if (!strcmp(route, "/api/status")) {
            rc = playback_snapshot(&out);
        } else if (!strcmp(route, "/api/tv/input")) {
            rc = tv_input(&out);
        } else if (!strcmp(route,"/api/youtube/status")) {
            rc=yt_status(&out);
        } else if (!strcmp(route,"/api/youtube/search")) {
            rc=yt_search(query,&out);
        } else if (!strcmp(route,"/api/youtube/state")) {
            rc=yt_state(NULL,&out);
        } else if (!strcmp(route, "/api/iptv/state")) {
            rc = iptv_state(NULL,&out);
        } else if (!strcmp(route, "/api/iptv/status")) {
            rc = iptv_status(&out);
        } else if (!strcmp(route,"/api/iptv/groups") || !strcmp(route,"/api/iptv/channels")) {
            rc = iptv_library_api(route,query,&out);
        } else if (!strcmp(route, "/api/frigate/cameras")) {
            rc = frigate_cameras(&out);
        } else if (!strcmp(route, "/api/auth/status")) {
            state_lock();
            auth_status_locked(&out);
            state_unlock();
        } else if (!strcmp(route, "/api/auth/poll")) {
            rc = qc_poll(&out);
        } else if (!strcmp(route, "/api/libraries")) {
            rc = jf_libraries(&out);
        } else if (!strcmp(route, "/api/items")) {
            rc = jf_items(query, &out);
        } else if (!strcmp(route, "/api/jellyfin/resume")) {
            if (require_token()) rc = -1;
            else {
                struct jval *root = NULL, *items = NULL; double total = 0;
                char offset[16] = ""; query_param(query, "offset", offset, sizeof offset);
                int start = atoi(offset); if (start < 0) start = 0;
                rc = items_query("Movie,Episode,Video", "IsResumable", 1, 60, start, NULL, NULL, &root, &items, &total);
                if (!rc) {
                    sb_fmt(&out, "{\"total\":%.0f,\"items\":[", total);
                    for (size_t i = 0; jnth(items, i); i++) { if (i) sb_puts(&out, ","); items_append(&out, jnth(items, i)); }
                    sb_puts(&out, "]}");
                }
                jfree(root);
            }
        } else if (!strcmp(route, "/api/tv/state")) {
            tv_state_load(&out);
        } else if (!strcmp(route, "/api/doom/status")) {
            api_doom_status(fd);
            jfree(payload);
            return;
        } else if (!strcmp(route, "/api/doom/input")) {
            struct sb b = {0};
            sb_puts(&b, "{\"keys\":");
            char num[32];
            snprintf(num, sizeof num, "%u", doom_input_read());
            sb_puts(&b, num);
            sb_puts(&b, "}");
            send_json_sb(fd, 200, "OK", &b, "GET");
            free(b.p);
            jfree(payload);
            return;
        } else {
            send_json_error(fd, 404, "not found", method);
            jfree(payload);
            return;
        }
    } else if (!strcmp(method, "POST")) {
        if (!strcmp(route, "/api/settings")) {
            struct jval *quality = jget(payload, "jellyfinVideoBitrate");
            double value = jnum(quality, 0);
            if (payload->n != 1 || !quality || quality->t != J_NUM || jf_quality_index(value) < 0) {
                send_json_error(fd, 400, "choose a listed Jellyfin video quality", method);
                jfree(payload); return;
            }
            state_lock();
            rc = jf_quality_save_locked((int)value);
            if (!rc) jf_settings_locked(&out);
            state_unlock();
        } else if (!strcmp(route, "/api/playback/pause") || !strcmp(route, "/api/playback/resume") || !strcmp(route, "/api/playback/stop")) {
            state_lock(); struct stream_slot *gate = active_slot_locked();
            int playing = S->playing, live = S->play_live, draining = gate && gate->draining; state_unlock();
            const char *action = strrchr(route, '/') + 1;
            if (!playing || ((live || draining) && strcmp(action, "stop"))) {
                send_json_error(fd, 409, playing ? "transport unsupported for this source" : "nothing is playing", method);
                jfree(payload); return;
            }
            rc = transport_action(action, &out);
        } else if (!strcmp(route, "/api/system/frontend/prepare")) {
            if (!native_frontend) {
                send_json_error(fd, 409, "native frontend mode required", method);
                jfree(payload); return;
            }
            if (payload->n) {
                send_json_error(fd, 400, "no parameters accepted", method);
                jfree(payload); return;
            }
            rc = native_frontend_prepare(&out);
        } else if (!strcmp(route, "/api/tv/exit")) {
            if (!native_frontend) {
                rc = run_shell(NULL, 0, 15,
                "/opt/middleware_core/system/tv/uconntest '<com.ucentric.pvruconnect.DirectTest command=\"itvStopApp\" sessionId=\"0\"/>' 2>&1");
            /* Vendor helper exit codes are not POSIX success statuses. */
            rc = rc_itv_running() == 0 ? 0 : fail("ITV did not exit");
            }
            sb_puts(&out, "{\"stopped\":true}");
        } else if (!strcmp(route, "/api/auth/start")) {
            rc = qc_start(&out);
        } else if (!strcmp(route, "/api/auth/logout")) {
            auth_logout(&out);
        } else if (!strcmp(route,"/api/youtube/play")) {
            if (!yt_valid_id(jstr(jget(payload,"videoId")))) {
                send_json_error(fd, 400, "Invalid video ID", method); jfree(payload); return;
            }
            rc=yt_play(jstr(jget(payload,"videoId")),&out);
        } else if (!strcmp(route,"/api/youtube/state")) {
            rc=yt_state(payload,&out);
        } else if (!strcmp(route,"/api/youtube/stop")) {
            state_lock();int active=S->playing&&S->play_live==3;S->yt_active=0;state_unlock();
            if(active)rc=transport_action("stop",&out);else sb_puts(&out,"{\"stopped\":true}");
        } else if (!strcmp(route, "/api/iptv/state")) {
            state_lock();rc=iptv_state(payload,&out);state_unlock();
        } else if (!strcmp(route, "/api/iptv/stop")) {
            state_lock();int live_iptv=S->playing&&S->play_live==2;
            if(!live_iptv)S->iptv_active=0;state_unlock();
            if(live_iptv)rc=transport_action("stop",&out);else sb_puts(&out,"{\"stopped\":true}");
        } else if (!strcmp(route, "/api/iptv/play")) {
            if (!iptv_find(jstr(jget(payload,"channelId")))) {
                send_json_error(fd, 404, "Unknown IPTV channel ID", method); jfree(payload); return;
            }
            rc = iptv_play(jstr(jget(payload,"channelId")),&out);
        } else if (!strcmp(route, "/api/frigate/play")) {
            const char *camera = jstr(jget(payload, "cameraId"));
            if (!camera || !*camera || strlen(camera) > 128 || strspn(camera, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != strlen(camera)) {
                send_json_error(fd, 400, "invalid cameraId", method); jfree(payload); return;
            }
            rc = frigate_play(camera, &out);
        } else if (!strcmp(route, "/api/play")) {
            const char *item_id = jstr(jget(payload, "itemId"));
            double start = jnum(jget(payload, "startSeconds"), 0);
            int return_to_tv = jbool(jget(payload, "returnToTv"), 0);
            if (!item_id || !*item_id || strlen(item_id) >= JF_ID_MAX || strspn(item_id, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != strlen(item_id) || !isfinite(start) || start < 0 || start > 604800) {
                send_json_error(fd, 400, "invalid itemId or startSeconds", method);
                jfree(payload);
                return;
            }
            if (require_token()) {
                rc = -1;
            } else if (start_play(item_id, start, return_to_tv, &out)) {
                rc = -1;
            } else {
                sb_puts(&out, "}");
                rc = 0;
            }
        } else if (!strcmp(route, "/api/transport")) {
            const char *action = jstr(jget(payload, "action"));
            if (!action || (strcmp(action, "pause") && strcmp(action, "resume") && strcmp(action, "stop"))) {
                send_json_error(fd, 400, "unsupported transport action", method); jfree(payload); return;
            }
            rc = transport_action(action, &out);
        } else if (!strcmp(route, "/api/seek") || !strcmp(route, "/api/playback/seek")) {
            struct jval *seconds_arg = jget(payload, "seconds"), *delta_arg = jget(payload, "delta");
            if ((!seconds_arg && !delta_arg) || (seconds_arg && (seconds_arg->t != J_NUM || !isfinite(seconds_arg->num) || seconds_arg->num < 0 || seconds_arg->num > 604800)) ||
                (delta_arg && (delta_arg->t != J_NUM || !isfinite(delta_arg->num) || delta_arg->num < -604800 || delta_arg->num > 604800))) {
                send_json_error(fd, 400, "seconds or delta must be bounded numbers", method); jfree(payload); return;
            }
            state_lock();
            if (!S->playing) {
                state_unlock();
                send_json_error(fd, 409, "nothing is playing", method);
                jfree(payload);
                return;
            }
            if (S->play_live) {
                state_unlock(); send_json_error(fd, 409, "seek unsupported for this source", method);
                jfree(payload); return;
            }
            char item[JF_ID_MAX];
            snprintf(item, sizeof item, "%s", S->play_item);
            double target = playback_elapsed_locked();
            int return_to_tv = S->return_to_tv;
            playback_end_locked();
            state_unlock();
            struct jval *seconds = jget(payload, "seconds");
            if (seconds && seconds->t == J_NUM) target = seconds->num;
            target += jnum(jget(payload, "delta"), 0);
            if (target < 0) target = 0;
            jf_log("seek to %d", (int)target);
            if (require_token()) {
                rc = -1;
            } else if (start_play(item, target, return_to_tv, &out)) {
                rc = -1;
            } else {
                sb_puts(&out, "}");
                rc = 0;
            }
        } else if (!strcmp(route, "/api/tv/state")) {
            state_lock();
            tv_state_save_locked(payload);
            state_unlock();
            sb_puts(&out, "{\"ok\":true}");
        } else if (!strcmp(route, "/api/tv/event")) {
            tv_event_record(payload);
            sb_puts(&out, "{\"ok\":true}");
        } else if (!strcmp(route, "/api/doom/input")) {
            if (!doom_armed()) {
                send_json_error(fd, 409, "doom not armed", method);
                jfree(payload);
                return;
            }
            doom_input_write((unsigned)jnum(jget(payload, "keys"), 0));
            sb_puts(&out, "{\"ok\":true}");
        } else if (!strcmp(route, "/api/doom/caps")) {
            /* The TV frontend reports what its JavaScript engine actually
               supports.  Script errors are only visible in the receiver's
               own log, never on the page, so this is the only way to find
               out which canvas features are usable before relying on them. */
            jf_log("DOOM caps: arrayBuffer=%s typed=%s imgData=%s caps=%s err=%s state=%s",
                   jstr(jget(payload, "arrayBuffer")),
                   jstr(jget(payload, "typed")),
                   jstr(jget(payload, "imgData")),
                   jstr(jget(payload, "caps")),
                   jstr(jget(payload, "err")),
                   jstr(jget(payload, "state")));
            sb_puts(&out, "{\"ok\":true}");
        } else if (!strcmp(route, "/api/doom/start")) {
            if (!native_frontend) { send_json_error(fd, 409, "native frontend mode required", method); jfree(payload); return; }
            rc = native_doom_start(&out);
        } else if (!strcmp(route, "/api/doom/stop")) {
            doom_stop("requested by frontend");
            if (native_frontend && native_doom_busy()) rc = fail("Doom clean shutdown incomplete");
            else sb_puts(&out, "{\"ok\":true,\"running\":false}");
        } else {
            send_json_error(fd, 404, "not found", method);
            jfree(payload);
            return;
        }
    } else {
        send_json_error(fd, 405, "Method Not Allowed", method);
        jfree(payload);
        return;
    }

    if (rc) send_json_error(fd, 502, g_err, method);
    else send_json_sb(fd, 200, "OK", &out, method);
    free(out.p);
    jfree(payload);
}

/* ------------------------------------------------------------------ */
/* Request dispatch                                                  */
/* ------------------------------------------------------------------ */

static void handle(int client) {
    char request[REQ_CAP];
    size_t used = 0;
    char *end = NULL;
    while (used < sizeof request - 1) {
        ssize_t n = read(client, request + used, sizeof request - 1 - used);
        if (n <= 0) return;
        used += (size_t)n;
        request[used] = 0;
        end = strstr(request, "\r\n\r\n");
        if (end) break;
    }
    if (!end) { send_json_error(client, 431, "Headers Too Large", "GET"); return; }
    char method[8], path[2048];
    if (sscanf(request, "%7s %2047s", method, path) != 2) {
        send_json_error(client, 400, "Bad Request", "GET");
        return;
    }
    if (strcmp(method, "GET") && strcmp(method, "HEAD") && strcmp(method, "POST")) {
        send_json_error(client, 405, "Method Not Allowed", method); return;
    }
    size_t length = 0;
    char *line = strstr(request, "\r\n");
    while (line && line < end) {
        line += 2;
        if (!strncasecmp(line, "Content-Length:", 15))
            length = (size_t)strtoul(line + 15, NULL, 10);
        line = strstr(line, "\r\n");
    }
    if (length > BODY_CAP) { send_json_error(client, 413, "Body Too Large", method); return; }
    char *body = end + 4;
    size_t have = used - (size_t)(body - request);
    while (have < length) {
        if (used >= sizeof request - 1) break;
        ssize_t n = read(client, request + used, sizeof request - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        request[used] = 0;
        have = used - (size_t)(body - request);
    }
    if (have > length) have = length;
    body[have] = 0;

    char path_only[2048];
    snprintf(path_only, sizeof path_only, "%s", path);
    char *query = strchr(path_only, '?');
    if (query) *query++ = 0;

    if (!strncmp(path_only,"/youtube-stream/",16)) {yt_stream(client,path_only+16,method);return;}
    if (!strncmp(path_only, "/iptv-stream/", 13)) {
        iptv_stream(client,path_only+13,method);return;
    }
    if (!strncmp(path_only, "/play/", 6)) {
        serve_play(client, path_only + 6);
        return;
    }
    if (!strcmp(path_only, "/doom/frame")) {
        serve_doom_frame(client, method, query);
        return;
    }
    if (!strncmp(path_only, "/api/", 5)) {
        if (!strcmp(path_only, "/api/play") || !strcmp(path_only, "/api/transport") ||
            !strcmp(path_only, "/api/seek") || !strncmp(path_only, "/api/playback/", 14) || !strncmp(path_only, "/api/frigate/", 13) || !strncmp(path_only,"/api/iptv/",10) || !strncmp(path_only,"/api/youtube/",13))
            alarm(240);
        handle_api(client, method, path_only, query, body);
        return;
    }
    if (!strncmp(path_only, "/art/", 5)) {
        int native_size = !strncmp(path_only, "/art/native/", 12);
        serve_art(client, method, path_only + (native_size ? 12 : 5), native_size);
        return;
    }
    if (strcmp(method, "GET") && strcmp(method, "HEAD")) {
        send_json_error(client, 405, "Method Not Allowed", method);
        return;
    }
    serve_static(client, method, path_only);
}

/* ------------------------------------------------------------------ */
/* Menu watcher, ported from server.py's watch_tv_launcher            */
/* ------------------------------------------------------------------ */

static int hijack_paused(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/ui/hijack-pause", persist_root);
    struct stat st;
    return stat(path, &st) == 0;
}

/* ------------------------------------------------------------------ */
/* Physical GUIDE detection via the platform KEYTRACK log              */
/* ------------------------------------------------------------------ */
/* Druid consumes the GUIDE key before ITV/WebKit ever sees it: the raw
 * code is dispatched, but no DOM keydown is produced and the screen id
 * does not change.  Waiting on the WebView can therefore never work.
 *
 * Instead we tail the receiver's own key dispatcher log, which records
 * every remote press as it happens, and match the raw IR code directly.
 * The mapping was recovered from the SHEF KeyTranslator and confirmed
 * against physical presses:
 *
 *   up=e100  down=e101  left=e102  right=e103  guide=e00b  info=e00e
 *   ok=e402
 *
 * A press is logged as "rawkey=1e00b" and the matching release as
 * "rawkey=e00b", so only the press form is matched.  This needs no
 * vendor library, no dlopen, and no new files on the box. */

#define KEYTRACK_LOG "/var/viewer/scan.log"
#define GUIDE_PRESS_TOKEN "rawkey=1e00b"

static int keytrack_fd = -1;
static off_t keytrack_off = 0;
static ino_t keytrack_ino = 0;

static void keytrack_open_tail(void) {
    if (keytrack_fd >= 0) return;
    struct stat st;
    int fd = open(KEYTRACK_LOG, O_RDONLY);
    if (fd < 0) return;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        return;
    }
    /* Start at end-of-file: only presses from now on are interesting. */
    keytrack_fd = fd;
    keytrack_ino = st.st_ino;
    keytrack_off = st.st_size;
}

static void keytrack_close(void) {
    if (keytrack_fd >= 0) close(keytrack_fd);
    keytrack_fd = -1;
    keytrack_off = 0;
    keytrack_ino = 0;
}

/* Returns 1 when a GUIDE press was seen since the previous call. */
static int keytrack_poll_guide(void) {
    if (keytrack_fd < 0) return 0;
    struct stat st;
    if (fstat(keytrack_fd, &st)) {
        close(keytrack_fd);
        keytrack_fd = -1;
        return 0;
    }
    /* Rotation or truncation: reopen and resume at the new end. */
    if (st.st_ino != keytrack_ino || st.st_size < keytrack_off) {
        close(keytrack_fd);
        keytrack_fd = -1;
        keytrack_off = 0;
        keytrack_ino = 0;
        keytrack_open_tail();
        return 0;
    }
    if (st.st_size == keytrack_off) return 0;
    char buf[4096];
    ssize_t got = pread(keytrack_fd, buf, sizeof buf - 1, keytrack_off);
    if (got <= 0) return 0;
    buf[got] = 0;
    off_t begin = keytrack_off;
    keytrack_off += got;
    state_lock();
    off_t skip = S->setup_key_inode == st.st_ino ? S->setup_key_offset - begin : 0;
    state_unlock();
    if (skip < 0) skip = 0;
    if (skip > got) skip = got;
    const char *controls = buf + skip;
    return (strstr(buf, GUIDE_PRESS_TOKEN) ? 1 : 0) |
           (strstr(controls, "rawkey=1e502") ? 2 : 0) |
           ((strstr(controls, "rawkey=1e402") || strstr(controls, "rawkey=e402")) ? 4 : 0);
}

/* ------------------------------------------------------------------ */
/* DOOM kill switch                                                    */
/* ------------------------------------------------------------------ */
/* Everything DOOM-related is inert unless the arming file exists:
 *
 *   $persist_root/doom/ENABLED
 *
 * The Media Hub also uses the key log to bridge EXIT and playback STOP.
 * DOOM-specific GUIDE handling still requires this arming file.  Removing the file while
 * an engine is running also stops it, which is the fast way out if a
 * rendering attempt destabilises the window manager.
 *
 *   $persist_root/doom/DISABLED   hard off, wins over ENABLED
 *
 * The running engine is recorded in doom.pid so it can be stopped without
 * matching on the process name. */

/* Returns 0 on success, -1 if the path would not fit. */
static int doom_path(char *out, size_t n, const char *rel) {
    int k = snprintf(out, n, "%s/doom/%s", persist_root, rel);
    return (k < 0 || (size_t)k >= n) ? -1 : 0;
}

static int doom_armed(void) {
    char path[512];
    struct stat st;
    if (doom_path(path, sizeof path, "DISABLED") == 0 && stat(path, &st) == 0)
        return 0;
    if (doom_path(path, sizeof path, "ENABLED") || stat(path, &st)) return 0;
    return 1;
}

static int doom_running_pid(void) {
    char path[512];
    if (doom_path(path, sizeof path, "doom.pid")) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int pid = -1;
    if (fscanf(f, "%d", &pid) != 1) pid = -1;
    fclose(f);
    if (pid <= 1) return -1;
    if (kill(pid, 0) && errno == ESRCH) return -1;
    if (native_frontend) {
        char proc[64], exe[512], expected[512];
        snprintf(proc, sizeof proc, "/proc/%d/exe", pid);
        ssize_t n = readlink(proc, exe, sizeof exe - 1);
        if (n < 0) return -1;
        exe[n] = 0;
        doom_path(expected, sizeof expected, "bin/hr54-doom-native");
        if (strcmp(exe, expected)) return -1; /* Never signal a reused PID. */
    }
    return pid;
}

/* The lock remains held through initialization and reaping. A disappeared
 * PID alone does not prove its retained graphics surface has been released. */
static int native_doom_busy(void) {
    if (!native_frontend) return doom_running_pid() > 0;
    char path[512];if (doom_path(path, sizeof path, "native.lock")) return 1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno != ENOENT;
    int busy = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);return busy;
}

/* Stop a running engine.  Safe to call when nothing is running. */
static void doom_stop(const char *why) {
    int pid = doom_running_pid();
    if (pid < 0) return;
    jf_log("DOOM: stopping pid %d (%s)", pid, why);
    kill(pid, SIGTERM);
    for (int i = 0; i < 20 && (native_frontend ? native_doom_busy() : doom_running_pid() > 0); i++) nap(0.25);
    if (native_frontend ? native_doom_busy() : doom_running_pid() > 0) {
        if (native_frontend) { jf_log("DOOM: clean shutdown timed out; surface recovery requires inspection"); return; }
        kill(pid, SIGKILL);
        jf_log("DOOM: pid %d killed", pid);
    }
    char path[512];
    if (!doom_path(path, sizeof path, "doom.pid")) unlink(path);
}

/* Lifecycle belongs to the backend. Fixed executable and WAD, no caller paths
 * or arguments. A reaper owns the engine and removes its PID on normal exit. */
static int native_doom_start(struct sb *out) {
    char bin[512], wad[512], pidf[512], lock[512];
    doom_path(bin, sizeof bin, "bin/hr54-doom-native");
    doom_path(wad, sizeof wad, "data/doom1.wad");
    doom_path(pidf, sizeof pidf, "doom.pid");
    doom_path(lock, sizeof lock, "native.lock");
    if (!doom_armed()) return fail("Doom is disabled");
    if (doom_running_pid() > 0) { sb_puts(out, "{\"running\":true}"); return 0; }
    if (access(bin, X_OK) || access(wad, R_OK)) return fail("Native Doom assets unavailable");
    int lf = open(lock, O_CREAT | O_RDWR, 0600);
    if (lf < 0 || flock(lf, LOCK_EX | LOCK_NB)) { if (lf >= 0) close(lf); return fail("Doom lifecycle busy"); }
    int ready[2]; if (pipe(ready)) { close(lf); return fail("Doom launch pipe failed"); }
    pid_t reaper = fork();
    if (!reaper) {
        close(ready[0]); setsid(); signal(SIGCHLD, SIG_DFL); alarm(0);
        /* Do not hold the request connection while Doom runs. */
        for (int f = 3; f < 1024; f++) if (f != lf && f != ready[1]) close(f);
        int execpipe[2], initialized[2]; if (pipe(execpipe) || pipe(initialized)) _exit(1);
        fcntl(execpipe[1], F_SETFD, FD_CLOEXEC);
        pid_t engine = fork();
        if (!engine) {
            close(execpipe[0]); close(initialized[0]); close(lf); close(ready[1]);
            char fdarg[16]; snprintf(fdarg, sizeof fdarg, "%d", initialized[1]);
            execl(bin, "hr54-doom-native", "--depth", HR54_NATIVE_FOREGROUND_DEPTH_ARG, "--seconds", "0", "--ready-fd", fdarg, "-iwad", wad, (char *)NULL);
            int error = errno; write(execpipe[1], &error, sizeof error); _exit(127);
        }
        close(execpipe[1]); close(initialized[1]); int error = 0;
        ssize_t n = read(execpipe[0], &error, sizeof error); close(execpipe[0]);
        int recorded = 0;
        if (engine > 1 && n == 0) {
            FILE *p = fopen(pidf, "w");
            if (p) { fprintf(p, "%d\n", (int)engine); recorded = fclose(p) == 0; }
        }
        struct pollfd init = {initialized[0], POLLIN, 0}; unsigned char initialized_ok = 0;
        if (recorded && poll(&init, 1, 5000) > 0 && read(initialized[0], &initialized_ok, 1) == 1 && initialized_ok == 1) {
            write(ready[1], &engine, sizeof engine);
        } else if (engine > 1) kill(engine, SIGTERM);
        close(initialized[0]);
        if (engine > 1) { int status; while (waitpid(engine, &status, 0) < 0 && errno == EINTR) {} }
        unlink(pidf); close(lf); close(ready[1]); _exit(0);
    }
    close(ready[1]); close(lf);
    if (reaper < 0) { close(ready[0]); return fail("Doom launch failed"); }
    struct pollfd p = {ready[0], POLLIN, 0}; pid_t engine = 0;
    int rc = poll(&p, 1, 7000);
    ssize_t n = rc > 0 ? read(ready[0], &engine, sizeof engine) : -1; close(ready[0]);
    if (n != sizeof engine || engine <= 1) return fail("Native Doom failed to execute");
    sb_fmt(out, "{\"running\":true,\"pid\":%d}", (int)engine); return 0;
}

static int doom_engine_present(void) {
    char path[512];
    if (doom_path(path, sizeof path, "bin/hr54-doom")) return 0;
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IXUSR);
}

/* The frame pipeline is verifiable without a WAD: dropping this file in
 * makes the launcher start the engine in self-test mode, which paints a
 * known pattern instead of a game.  Used to prove transport, presentation
 * and the kill switch before the WAD is involved. */
static int doom_selftest(void) {
    char path[512];
    struct stat st;
    if (doom_path(path, sizeof path, "selftest")) return 0;
    return stat(path, &st) == 0;
}

/* Launch the DOOM engine.  The engine is a frame producer, not a screen
 * owner: it renders into a file that the TV frontend pulls over HTTP and
 * paints into a fullscreen canvas.  That keeps all drawing inside the
 * already-working WebKit surface, because no process outside the vendor
 * stack can load libdtvwm.so (its load-time init segfaults).
 *
 *   argv[1] WAD          patch to load
 *   argv[2] frame output file to publish atomically
 *   argv[3] input file to poll for a held-key bitmask
 */
static void rc_launch_doom(void) {
    char bin[512], wad[512], pidf[512], frame[512], input[512];
    doom_path(bin, sizeof bin, "bin/hr54-doom");
    doom_path(wad, sizeof wad, "data/doom1.wad");
    doom_path(pidf, sizeof pidf, "doom.pid");
    doom_path(frame, sizeof frame, "frame.raw");
    doom_path(input, sizeof input, "input");
    struct stat st;
    int self = doom_selftest();
    if (stat(wad, &st) && !self) {
        jf_log("GUIDE: DOOM wad missing at %s", wad);
        return;
    }
    if (!doom_engine_present()) {
        jf_log("GUIDE: DOOM engine missing at %s", bin);
        return;
    }
    if (doom_running_pid() > 0) return;
    jf_log("GUIDE: launching DOOM%s", self ? " (selftest)" : "");
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(bin, "hr54-doom", self ? "selftest" : wad, frame, input, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        FILE *f = fopen(pidf, "w");
        if (f) {
            fprintf(f, "%d\n", (int)pid);
            fclose(f);
        }
        jf_log("GUIDE: DOOM pid %d", (int)pid);
    } else jf_log("GUIDE: fork failed");
}

/* ------------------------------------------------------------------ */
/* DOOM frame transport                                               */
/* ------------------------------------------------------------------ */
/* The engine never touches the TV.  It publishes finished frames to
 * doom/frame.raw and the TV frontend paints them, which keeps every
 * drawing operation inside the WebKit surface that already works.
 *
 * Frame file layout, all integers little-endian so the browser can read
 * them with DataView without knowing the box is big-endian:
 *
 *   0    "HRDF"                 magic
 *   4    u32 version            1
 *   8    u32 width
 *   12   u32 height
 *   16   u32 seq                increments once per published frame
 *   20   256 * 4 bytes palette  R,G,B,A per entry
 *   1044 width*height bytes     one palette index per pixel
 *
 * The engine renames a fully written temporary over the real name, so a
 * reader either sees a whole old frame or a whole new one, never a tear.
 *
 * Transport is base64 text rather than binary because the ITV WebKit is
 * old enough that XHR responseType="arraybuffer" is not dependable, and
 * text/plain is universally safe.  atob() on the far side undoes it. */

#define DOOM_FRAME_MAGIC "HRDF"
#define DOOM_FRAME_VER 1u
#define DOOM_FRAME_HDR 1044u

/* Held-key bitmask, mirrored in the TV frontend and read by the engine. */
#define DOOM_IN_LEFT    (1u << 0)
#define DOOM_IN_RIGHT   (1u << 1)
#define DOOM_IN_UP      (1u << 2)
#define DOOM_IN_DOWN    (1u << 3)
#define DOOM_IN_FIRE    (1u << 4)
#define DOOM_IN_USE     (1u << 5)
#define DOOM_IN_STRAFE_L (1u << 6)
#define DOOM_IN_STRAFE_R (1u << 7)

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encodes len bytes, allocating the result.  Returns NULL on overflow. */
static char *base64_encode(const unsigned char *in, size_t len) {
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = b64tab[(v >> 6) & 63];
        out[o++] = b64tab[v & 63];
        i += 3;
    }
    if (i < len) {
        unsigned v = (unsigned)in[i] << 16;
        int two = (i + 1 < len);
        if (two) v |= (unsigned)in[i + 1] << 8;
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = two ? b64tab[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return out;
}

static unsigned rd_le32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}

/* Reads the frame sequence number without decoding the pixels. */
static long doom_frame_seq(void) {
    char path[512];
    unsigned char hdr[20];
    if (doom_path(path, sizeof path, "frame.raw")) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, hdr, sizeof hdr);
    close(fd);
    if (n != (ssize_t)sizeof hdr) return -1;
    if (memcmp(hdr, DOOM_FRAME_MAGIC, 4) || rd_le32(hdr + 4) != DOOM_FRAME_VER)
        return -1;
    return (long)rd_le32(hdr + 16);
}

/* GET /doom/frame[?since=N]
 *
 * Sends the published frame as base64, or a bodiless 304 when the caller
 * already has it.  The 304 is the common case: the frontend polls much
 * faster than the engine renders, and not re-sending an unchanged frame
 * keeps the LAN link and the browser's decoder idle. */
static void serve_doom_frame(int fd, const char *method, const char *query) {
    if (!doom_armed()) { send_json_error(fd, 404, "Not Found", method); return; }
    char path[512];
    if (doom_path(path, sizeof path, "frame.raw")) {
        send_json_error(fd, 404, "Not Found", method);
        return;
    }
    long since = -1;
    if (query) {
        const char *s = strstr(query, "since=");
        if (s) since = strtol(s + 6, NULL, 10);
    }
    long seq = doom_frame_seq();
    if (seq < 0) { send_json_error(fd, 404, "Not Found", method); return; }
    if (since >= 0 && since == seq) {
        state_lock();
        if (S) S->doom_frames_304++;
        state_unlock();
        send_body(fd, 304, http_reason(304), "text/plain; charset=utf-8",
                  "no-store, no-cache, must-revalidate", "", 0, method);
        return;
    }
    size_t len = 0;
    unsigned char *raw = (unsigned char *)read_file(path, &len);
    if (!raw) { send_json_error(fd, 404, "Not Found", method); return; }
    char *b64 = base64_encode(raw, len);
    free(raw);
    if (!b64) { send_json_error(fd, 500, "Internal Server Error", method); return; }
    state_lock();
    if (S) S->doom_frames_served++;
    state_unlock();
    send_body(fd, 200, "OK", "text/plain; charset=utf-8",
              "no-store, no-cache, must-revalidate", b64, strlen(b64), method);
    free(b64);
}

/* Held-key bitmask the engine polls.  Written atomically so the engine
 * never reads a half-updated value. */
static unsigned doom_input_read(void) {
    char path[512];
    if (doom_path(path, sizeof path, "input")) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned mask = 0;
    if (fscanf(f, "%u", &mask) != 1) mask = 0;
    fclose(f);
    return mask & 0xffu;
}

static void doom_input_write(unsigned mask) {
    char path[512], tmp[512];
    if (doom_path(path, sizeof path, "input")) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%u\n", mask & 0xffu);
    fclose(f);
    if (rename(tmp, path)) unlink(tmp);
}

static void api_doom_status(int fd) {
    long seq = doom_frame_seq();
    int pid = doom_running_pid();
    struct sb b = {0};
    sb_fmt(&b, "{\"native\":%s,\"armed\":", native_frontend ? "true" : "false");
    sb_puts(&b, doom_armed() ? "true" : "false");
    sb_puts(&b, ",\"running\":");
    sb_puts(&b, (native_frontend ? native_doom_busy() : pid > 0) ? "true" : "false");
    sb_puts(&b, ",\"pid\":");
    char num[32];
    snprintf(num, sizeof num, "%d", pid > 0 ? pid : 0);
    sb_puts(&b, num);
    sb_puts(&b, ",\"seq\":");
    snprintf(num, sizeof num, "%ld", seq < 0 ? -1 : seq);
    sb_puts(&b, num);
    state_lock();
    unsigned long served = S ? S->doom_frames_served : 0;
    unsigned long notmod = S ? S->doom_frames_304 : 0;
    state_unlock();
    sb_puts(&b, ",\"served\":");
    snprintf(num, sizeof num, "%lu", served);
    sb_puts(&b, num);
    sb_puts(&b, ",\"notModified\":");
    snprintf(num, sizeof num, "%lu", notmod);
    sb_puts(&b, num);
    sb_puts(&b, "}");
    send_json_sb(fd, 200, "OK", &b, "GET");
    free(b.p);
}

static void watcher_run(void) {
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    alarm(0);
    /* Token validation can open this before fork().  Give the watcher its
     * own open-file description so flock actually excludes sibling workers. */
    if (lock_fd_local >= 0) close(lock_fd_local);
    lock_fd_local = -1;
    double stable_since = 0, app_seen_at = 0, launched_at = 0;
    int stable_screen = -2;
    int doom_was_armed = 0;
    int probe_was_ok = 1;
    for (;;) {
        double now = mono_now();
        int armed = doom_armed();
        if (armed != doom_was_armed) {
            jf_log("DOOM: %s", armed ? "armed" : "disarmed");
            if (!armed) {
                doom_stop("kill switch");
                keytrack_close();
            } else {
                keytrack_open_tail();
            }
            doom_was_armed = armed;
        }
        keytrack_open_tail();
        int remote_keys = keytrack_poll_guide();
        if (armed) {
            if (remote_keys & 1) {
                jf_log("GUIDE press detected (raw e00b)");
                rc_launch_doom();
            }
        }
        int screen = -1, boot_osd = 0, probe_ok = 1;
        if (rc_probe(&screen, &boot_osd)) {
            if (probe_was_ok) jf_log("MENU watcher screen probe failed");
            probe_was_ok = 0;
            probe_ok = 0;
            screen = -1;
        } else if (!probe_was_ok) {
            jf_log("MENU watcher screen probe recovered");
            probe_was_ok = 1;
        }
        /* Only a successful probe may move the stability clock.  Druid drops
         * the occasional sample while the stack is busy, and treating that as
         * a screen change both spammed the log and kept resetting the idle
         * timer, which delayed the takeover by seconds. */
        if (probe_ok && screen != stable_screen) {
            stable_screen = screen;
            stable_since = now;
            jf_log("MENU watcher screen %d", screen);
            if (screen == MENU_SCREEN_A || screen == MENU_SCREEN_B)
                jf_log_ms("MENU stock menu screen %d detected", screen);
        }
        state_lock();
        int playing = S->playing;
        if(S->yt_active&&!playing&&(remote_keys&6))S->yt_active=0;
        state_unlock();
        yt_maybe_update();
        int in_menu = screen == MENU_SCREEN_A || screen == MENU_SCREEN_B;
        state_lock();
        int key_ready = now >= S->ui_key_until;
        state_unlock();
        if (playing && key_ready && (remote_keys & 6)) {
            struct sb stopped = {0}; transport_action("stop", &stopped); free(stopped.p);
            jf_log("remote STOP/EXIT returned playback to its source");
            nap(1); continue;
        }
        if (playing && !in_menu) { nap(1.0); continue; }
        int idle = now - stable_since >=
                   (playing ? REMOTE_EXIT_IDLE : MENU_IDLE_SECONDS);
        int app_running = rc_itv_running();
        if (app_running < 0) {
            jf_log("MENU watcher ITV status probe failed");
            nap(2.0);
            continue;
        }
        if (!playing && app_running) app_seen_at = now;

        if (!playing && app_running && in_menu) {
            /* MENU is also handled by Druid even while ITV owns the screen.
             * A second press can therefore leave the stock menu composited
             * over the still-running Jellyfin app.  Do not restart WebKit --
             * just walk Druid back to LiveTV so the existing app is revealed
             * with its navigation state intact. */
            jf_log("stock menu covered Jellyfin; dismissing it");
            if (rc_walk_to_live_tv(screen))
                jf_log("stock menu dismissal failed");
            stable_screen = LIVE_TV_SCREEN;
            stable_since = mono_now();
        } else if (playing && in_menu && idle) {
            state_lock();
            int return_to_tv = S->return_to_tv;
            playback_end_locked();
            state_unlock();
            jf_log("receiver left playback for a menu; ending stream");
            /* The Broadcom decoder can surrender the video plane several
             * seconds after the menu transition.  Relaunch only after the
             * normal decoder-dismiss interval so Druid cannot cover ITV
             * again with the still-draining menu plane. */
            schedule_tv_return(return_to_tv, POST_STOP_DISMISS);
            launched_at = now;
        } else if (!playing && in_menu && idle && !hijack_paused() &&
                   now - app_seen_at >= APP_EXIT_GRACE &&
                   now - launched_at >= LAUNCH_COOLDOWN && !app_running) {
            jf_log("MENU watcher presenting Jellyfin");
            jf_log_ms("menu stable for %.3fs; starting takeover", now - stable_since);
            rc_takeover_from_menu(screen, boot_osd);
            launched_at = mono_now();
        }
        /* Back off when Druid is not answering: hammering an unresponsive
         * middleware stack is what turned a transient blip into log spam. */
        nap(probe_ok ? WATCH_POLL : 2.0);
    }
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static int mkdir_p_mode(const char *path, mode_t mode) {
    char copy[512];
    if (snprintf(copy, sizeof copy, "%s", path) >= (int)sizeof copy) return -1;
    for (char *p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(copy, mode) && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(copy, mode) && errno != EEXIST) return -1;
    return chmod(path, mode);
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s DOCROOT JELLYFIN_IPV4 JELLYFIN_PORT "
                        "LISTEN_PORT [--no-launcher]\n", argv[0]);
        return 2;
    }
    for(int inherited=3;inherited<1024;inherited++)close(inherited);
    docroot = argv[1];
    const char *jf_host = argv[2];
    int jf_port = atoi(argv[3]);
    int listen_port = atoi(argv[4]);
    int run_launcher = 1;
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--no-launcher")) run_launcher = 0;
        if (!strcmp(argv[i], "--native-frontend")) { native_frontend = 1; run_launcher = 0; }
    }
    struct in_addr probe;
    if (inet_pton(AF_INET, jf_host, &probe) != 1 || jf_port <= 0 || listen_port <= 0) {
        fprintf(stderr, "invalid Jellyfin host/port or listen port\n");
        return 2;
    }
    const char *env;
    if ((env = getenv("JF_PERSIST_ROOT")) && *env) persist_root = env;
    if ((env = getenv("JF_PLAY_CMD")) && *env) play_cmd = env;
    /* The existing persistent indexer launcher keeps its command line. An
     * explicit deployment marker selects the native frontend and its guarded
     * boot supervisor, without changing the stock plugin or flash. */
    char native_boot_helper[512], native_enabled[512], native_activated[512];
    snprintf(native_boot_helper, sizeof native_boot_helper, "%s/../native-menu/bootstrap.sh", persist_root);
    snprintf(native_enabled, sizeof native_enabled, "%s/../native-menu/ENABLED", persist_root);
    snprintf(native_activated, sizeof native_activated, "%s/../native-menu/ACTIVATED", persist_root);
    int native_boot = !access(native_enabled, R_OK) && !access(native_activated, R_OK) && !access(native_boot_helper, X_OK);
    if (native_boot) { native_frontend = 1; run_launcher = 0; }

    S = mmap(NULL, sizeof *S, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (S == MAP_FAILED) { perror("mmap"); return 1; }
    memset(S, 0, sizeof *S);
    snprintf(S->jf_host, sizeof S->jf_host, "%s", jf_host);
    S->jf_port = jf_port;
    S->listen_port = listen_port;

    char sub[512];
    snprintf(sub, sizeof sub, "%s/config", persist_root);
    if (mkdir_p_mode(sub, 0700)) { perror(sub); return 1; }
    snprintf(sub, sizeof sub, "%s/state", persist_root);
    if (mkdir_p_mode(sub, 0700)) { perror(sub); return 1; }
    snprintf(sub, sizeof sub, "%s/cache", persist_root);
    if (mkdir_p_mode(sub, 0700)) { perror(sub); return 1; }
    snprintf(sub, sizeof sub, "%s/log", persist_root);
    if (mkdir_p_mode(sub, 0700)) { perror(sub); return 1; }

    /* Receiver LAN address, discovered the way the reference backend does. */
    {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd >= 0) {
            struct sockaddr_in to;
            memset(&to, 0, sizeof to);
            to.sin_family = AF_INET;
            to.sin_port = htons((unsigned short)jf_port);
            inet_pton(AF_INET, jf_host, &to.sin_addr);
            if (!connect(fd, (struct sockaddr *)&to, sizeof to)) {
                struct sockaddr_in me;
                socklen_t mel = sizeof me;
                if (!getsockname(fd, (struct sockaddr *)&me, &mel))
                    inet_ntop(AF_INET, &me.sin_addr, S->lan_addr, sizeof S->lan_addr);
            }
            close(fd);
        }
    }
    if (!S->lan_addr[0]) snprintf(S->lan_addr, sizeof S->lan_addr, "127.0.0.1");

    if(run_launcher || native_frontend)iptv_clock_bootstrap();
    iptv_reload();
    load_persist();
    jf_log("service starting: jellyfin %s:%d listen %d lan %s",
           S->jf_host, S->jf_port, listen_port, S->lan_addr);
    if (S->token[0]) jf_validate_token();

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in local;
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_port = htons((unsigned short)listen_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, (struct sockaddr *)&local, sizeof local) ||
        listen(listener, 12)) {
        perror("listen");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    if (native_boot) {
        pid_t boot = fork();
        if (!boot) {
            close(listener); for (int i = 3; i < 1024; i++) close(i);
            int null = open("/dev/null", O_RDWR);
            if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); if (null > 2) close(null); }
            execl(native_boot_helper, native_boot_helper, (char *)NULL); _exit(127);
        }
        if (boot > 0) jf_log("native boot supervisor pid %ld", (long)boot);
        else jf_log("native boot supervisor failed to fork: %s", strerror(errno));
    }
    if (run_launcher) {
        /* The persistent string bind arrives after Druid's first parse.
         * Optional helper performs one guarded reload per kernel boot, then
         * exits. Keep the listener, media state and existing boot image intact. */
        char flag[512],helper[512];
        snprintf(flag,sizeof flag,"%s/../overlays/strings-boot.enabled",persist_root);
        snprintf(helper,sizeof helper,"%s/../bin/hr54-apply-strings",persist_root);
        if(!access(flag,R_OK)&&!access(helper,X_OK)){
            pid_t strings=fork();
            if(!strings){
                close(listener);for(int i=3;i<1024;i++)close(i);
                int null=open("/dev/null",O_RDWR);if(null>=0){dup2(null,0);dup2(null,1);dup2(null,2);if(null>2)close(null);}
                execl(helper,helper,"--boot",(char *)NULL);_exit(127);
            }
            if(strings>0)jf_log("boot string helper pid %ld",(long)strings);
        }
        pid_t supervisor=getpid();
        pid_t w = fork();
        if (w == 0) {
            prctl(PR_SET_PDEATHSIG,SIGTERM);
            if(getppid()!=supervisor)_exit(0);
            close(listener);
            watcher_run();
            _exit(0);
        }
    }
    signal(SIGCHLD, SIG_IGN);

    for (;;) {
        int client = accept(listener, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            continue;
        }
        iptv_reload();
        pid_t request_owner=getpid();
        pid_t child = fork();
        if (child == 0) {
            if(lock_fd_local>=0)close(lock_fd_local);
            lock_fd_local=-1;
            prctl(PR_SET_PDEATHSIG,SIGTERM);
            if(getppid()!=request_owner)_exit(0);
            signal(SIGCHLD, SIG_DFL);
            close(listener);
            alarm(90);
            handle(client);
            close(client);
            _exit(0);
        }
        close(client);
    }
}
