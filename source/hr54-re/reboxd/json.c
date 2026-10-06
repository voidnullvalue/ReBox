#include "core.h"
_Thread_local char g_err[256];

int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return -1;
}

double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void nap(double seconds) {
    struct timespec ts = {(time_t)seconds,
                          (long)((seconds - (time_t)seconds) * 1e9)};
    nanosleep(&ts, NULL);
}

int write_all_fd(int fd, const void *data, size_t len) {
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

int sb_grow(struct sb *b, size_t need) {
    if (need > 4*1024*1024 || b->len > 4*1024*1024-need) return fail("JSON buffer exceeds limit");
    if (b->p && b->len + need + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + need + 1) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) return fail("out of memory");
    b->p = p;
    b->cap = cap;
    return 0;
}

int sb_putn(struct sb *b, const char *s, size_t n) {
    if (sb_grow(b, n)) return -1;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
    return 0;
}

int sb_puts(struct sb *b, const char *s) { return sb_putn(b, s, strlen(s)); }

int sb_fmt(struct sb *b, const char *fmt, ...) {
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
int sb_json_str(struct sb *b, const char *s) {
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

int url_encode(struct sb *out, const char *s) {
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

size_t url_decode(char *dst, size_t dstsz, const char *src, size_t n) {
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
int query_param(const char *query, const char *name,
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

void jfree(struct jval *v) {
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

int jpush(struct jval *parent, char *key, struct jval *child) {
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

void jskip_ws(const char **p, const char *end) {
    while (*p < end && (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r'))
        (*p)++;
}

static _Thread_local unsigned json_depth, json_nodes;
struct jval *jparse_value(const char **p, const char *end);
struct jval *jparse_inner(const char **p, const char *end);

int jparse_hex4(const char **p, const char *end, unsigned *out) {
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

int utf8_emit(struct sb *b, unsigned cp) {
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
int jparse_string(struct sb *out, const char **p, const char *end) {
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
                if (!cp || (cp >= 0xD800 && cp <= 0xDFFF)) return -1;
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

struct jval *jnew(enum jtype t) {
    struct jval *v = calloc(1, sizeof *v);
    if (v) v->t = t;
    return v;
}

struct jval *jparse_value(const char **p, const char *end) {
    if (json_depth >= 32 || ++json_nodes > 8192) return NULL;
    ++json_depth; struct jval *v = jparse_inner(p, end); --json_depth; return v;
}
struct jval *jparse_inner(const char **p, const char *end) {
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
            if (!key.p) key.p = calloc(1, 1);
            for (size_t k=0; k<o->n; ++k) if (!strcmp(o->keys[k], key.p)) { free(key.p); jfree(o); return NULL; }
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
        if (!used || !isfinite(d)) return NULL;
        struct jval *v = jnew(J_NUM);
        if (!v) return NULL;
        v->num = d;
        *p += used;
        return v;
    }
    return NULL;
}

struct jval *json_parse(const char *s, size_t n) {
    if (!s || n > 4*1024*1024) return NULL;
    json_depth=0; json_nodes=0;
    const char *p = s, *end = s + n;
    struct jval *v = jparse_value(&p, end);
    if (!v) return NULL;
    jskip_ws(&p, end);
    if (p != end) { jfree(v); return NULL; }
    return v;
}

struct jval *jget(struct jval *obj, const char *key) {
    if (!obj || obj->t != J_OBJ) return NULL;
    for (size_t i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key)) return obj->items[i];
    return NULL;
}

struct jval *jnth(struct jval *arr, size_t i) {
    if (!arr || arr->t != J_ARR || i >= arr->n) return NULL;
    return arr->items[i];
}

const char *jstr(struct jval *v) { return v && v->t == J_STR ? v->str : NULL; }
double jnum(struct jval *v, double dflt) {
    return v && v->t == J_NUM ? v->num : dflt;
}
int jbool(struct jval *v, int dflt) {
    if (!v) return dflt;
    if (v->t == J_TRUE) return 1;
    if (v->t == J_FALSE || v->t == J_NULL) return 0;
    return dflt;
}
