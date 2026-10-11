#define _GNU_SOURCE
/*
 * Bounded non-blocking HTTP + WebSocket server. See jeopardy_net.h.
 *
 * Framing rules enforced here (client -> server):
 *   - client frames MUST be masked (RFC6455 5.1); unmasked -> close 1002
 *   - RSV bits must be zero; continuation state must be consistent
 *   - control frames must be FIN and <= 125 bytes
 *   - assembled messages, read buffers, response queues and header blocks are
 *     all capped, and sockets are non-blocking, so a slow peer is dropped
 *     rather than allowed to stall the loop.
 * Server -> client frames are unmasked text/close/pong.
 */
#include "jeopardy_net.h"
#include "../../../reboxd/core.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/un.h>

#define JPQ_NET_MAX_HTTP 16
#define JPQ_NET_MAX_LINE 4
#define JPQ_NET_MAX_CONN (JPQ_NET_MAX_HTTP + JPQ_NET_MAX_LINE)
#define JPQ_IN_MAX (256u * 1024u)
#define JPQ_OUT_MAX (2u * 1024u * 1024u)
#define JPQ_MSG_MAX (64u * 1024u)
#define JPQ_HEADER_MAX 16384u
#define JPQ_BODY_MAX 65536u
#define JPQ_LINE_MAX 8192u
#define JPQ_PING_MS 20000u
#define JPQ_IDLE_MS 45000u

enum { CONN_FREE = 0, CONN_HTTP = 1, CONN_LINE = 2 };

typedef struct {
    int fd;
    int kind;
    int is_ws;
    int closing;
    int close_code;
    char ip[64];
    char *in;
    size_t in_len, in_cap;
    char *out;
    size_t out_len, out_cap, out_off;
    size_t header_end;
    size_t body_len;
    int dispatched;
    char *frame;
    size_t frame_len, frame_cap;
    int frame_op;
    uint64_t last_rx, last_ping;
    JpqConnId id;
} Conn;

struct JpqNet {
    JpqNetOps ops;
    void *ud;
    int tcp_fd, unix_fd;
    int port;
    uint32_t next_id;
    Conn conns[JPQ_NET_MAX_CONN];
    volatile sig_atomic_t stop;
};

static uint64_t ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ------------------------------------------------------------------ */
/* SHA1 + base64 (RFC6455 handshake)                                   */
/* ------------------------------------------------------------------ */

static void sha1_block(uint32_t h[5], const unsigned char *block) {
    uint32_t w[80];
    for (int k = 0; k < 16; k++)
        w[k] = ((uint32_t)block[k * 4] << 24) | ((uint32_t)block[k * 4 + 1] << 16) |
               ((uint32_t)block[k * 4 + 2] << 8) | (uint32_t)block[k * 4 + 3];
    for (int k = 16; k < 80; k++) {
        uint32_t v = w[k - 3] ^ w[k - 8] ^ w[k - 14] ^ w[k - 16];
        w[k] = (v << 1) | (v >> 31);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int k = 0; k < 80; k++) {
        uint32_t f, g;
        if (k < 20) { f = (b & c) | ((~b) & d); g = 0x5A827999u; }
        else if (k < 40) { f = b ^ c ^ d; g = 0x6ED9EBA1u; }
        else if (k < 60) { f = (b & c) | (b & d) | (c & d); g = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; g = 0xCA62C1D6u; }
        uint32_t temp = ((a << 5) | (a >> 27)) + f + e + g + w[k];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1(const unsigned char *msg, size_t len, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint64_t bits = (uint64_t)len * 8u;
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha1_block(h, msg + i);
    unsigned char block[64];
    memset(block, 0, sizeof block);
    size_t rem = len - i;
    if (rem) memcpy(block, msg + i, rem);
    block[rem] = 0x80;
    if (rem >= 56) { sha1_block(h, block); memset(block, 0, sizeof block); }
    for (int k = 0; k < 8; k++) block[56 + k] = (unsigned char)(bits >> (56 - 8 * k));
    sha1_block(h, block);
    for (int k = 0; k < 5; k++) {
        out[k * 4] = (unsigned char)(h[k] >> 24);
        out[k * 4 + 1] = (unsigned char)(h[k] >> 16);
        out[k * 4 + 2] = (unsigned char)(h[k] >> 8);
        out[k * 4 + 3] = (unsigned char)h[k];
    }
}

static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const unsigned char *src, size_t len, char *out, size_t outcap) {
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)src[i] << 16;
        if (i + 1 < len) v |= (unsigned)src[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned)src[i + 2];
        if (o + 4 >= outcap) return 0;
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? b64tab[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? b64tab[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* ------------------------------------------------------------------ */
/* Buffer helpers                                                      */
/* ------------------------------------------------------------------ */

static void line_drain(JpqNet *n, Conn *c);

static int grow(char **buf, size_t *cap, size_t need) {
    if (*cap >= need) return 0;
    size_t ncap = *cap ? *cap : 512;
    while (ncap < need) ncap *= 2;
    char *p = realloc(*buf, ncap);
    if (!p) return -1;
    *buf = p;
    *cap = ncap;
    return 0;
}

static int conn_append(char **buf, size_t *len, size_t *cap, const void *data, size_t n, size_t max) {
    if (*len + n > max) return -1;
    if (grow(buf, cap, *len + n + 1)) return -1;
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = 0;
    return 0;
}

static void conn_release(Conn *c) {
    free(c->in); free(c->out); free(c->frame);
    c->in = c->out = c->frame = NULL;
    c->in_len = c->in_cap = c->out_len = c->out_cap = c->out_off = 0;
    c->frame_len = c->frame_cap = 0;
}

static Conn *conn_by_id(JpqNet *n, JpqConnId id) {
    for (int i = 0; i < JPQ_NET_MAX_CONN; i++)
        if (n->conns[i].kind != CONN_FREE && n->conns[i].id == id) return &n->conns[i];
    return NULL;
}

static void conn_close_now(JpqNet *n, Conn *c) {
    if (c->kind == CONN_FREE) return;
    int was_ws = c->is_ws;
    JpqConnId id = c->id;
    if (c->fd >= 0) close(c->fd);
    conn_release(c);
    c->fd = -1;
    c->kind = CONN_FREE;
    c->is_ws = 0;
    c->closing = 0;
    c->dispatched = 0;
    c->header_end = 0;
    c->body_len = 0;
    c->frame_op = 0;
    if (was_ws && n->ops.ws_close) n->ops.ws_close(n->ud, id, 0);
}

static void conn_set_closing(JpqNet *n, Conn *c, int code) {
    if (c->kind == CONN_FREE) return;
    if (!c->closing) { c->closing = 1; c->close_code = code; }
    (void)n;
}

/* ------------------------------------------------------------------ */
/* Output queue                                                        */
/* ------------------------------------------------------------------ */

static int conn_queue(JpqNet *n, Conn *c, const void *data, size_t len) {
    if (c->kind == CONN_FREE || c->closing) return -1;
    if (c->out_len - c->out_off + len > JPQ_OUT_MAX) {
        conn_set_closing(n, c, 1009);
        return -1;
    }
    if (conn_append(&c->out, &c->out_len, &c->out_cap, data, len, JPQ_OUT_MAX)) {
        conn_set_closing(n, c, 1009);
        return -1;
    }
    return 0;
}

static void conn_compact_out(Conn *c) {
    if (c->out_off == 0) return;
    if (c->out_off >= c->out_len) { c->out_len = 0; c->out_off = 0; return; }
    memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
    c->out_len -= c->out_off;
    c->out_off = 0;
}

static int conn_flush(JpqNet *n, Conn *c) {
    (void)n;
    while (c->out_off < c->out_len) {
        ssize_t w = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
        if (w > 0) { c->out_off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return -1;
    }
    conn_compact_out(c);
    return 0;
}

static void conn_reap(JpqNet *n) {
    for (int i = 0; i < JPQ_NET_MAX_CONN; i++) {
        Conn *c = &n->conns[i];
        if (c->kind == CONN_FREE) continue;
        if (c->closing && c->out_off >= c->out_len) conn_close_now(n, c);
    }
}

/* ------------------------------------------------------------------ */
/* WebSocket framing                                                   */
/* ------------------------------------------------------------------ */

static int ws_frame(JpqNet *n, Conn *c, int opcode, const void *payload, size_t len) {
    unsigned char hdr[10];
    size_t h = 0;
    hdr[h++] = (unsigned char)(0x80 | (opcode & 0x0f));
    if (len < 126) hdr[h++] = (unsigned char)len;
    else if (len <= 0xFFFF) { hdr[h++] = 126; hdr[h++] = (unsigned char)(len >> 8); hdr[h++] = (unsigned char)(len & 0xff); }
    else { hdr[h++] = 127; for (int k = 7; k >= 0; k--) hdr[h++] = (unsigned char)((uint64_t)len >> (8 * k)); }
    if (conn_queue(n, c, hdr, h)) return -1;
    if (len && conn_queue(n, c, payload, len)) return -1;
    return 0;
}

static void ws_protocol_error(JpqNet *n, Conn *c, int code) {
    unsigned char body[2];
    body[0] = (unsigned char)(code >> 8);
    body[1] = (unsigned char)(code & 0xff);
    ws_frame(n, c, 8, body, 2);
    conn_set_closing(n, c, code);
}

/* Strict RFC3629 UTF-8 validation (rejects overlong forms, surrogates and
 * codepoints above U+10FFFF) so invalid text frames close with 1007. */
static int utf8_valid(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i++];
        if (c < 0x80) continue;
        int need;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1Fu; if (cp < 2) return 0; }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07u; }
        else return 0;
        if (i + (size_t)need > n) return 0;
        for (int k = 0; k < need; k++) {
            unsigned char cc = s[i++];
            if ((cc & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (uint32_t)(cc & 0x3F);
        }
        if (need == 2 && (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))) return 0;
        if (need == 3 && (cp < 0x10000u || cp > 0x10FFFFu)) return 0;
    }
    return 1;
}

static void ws_dispatch(JpqNet *n, Conn *c, int op, const unsigned char *data, size_t len) {
    if (op == 1) {
        if (!utf8_valid(data, len)) { ws_protocol_error(n, c, 1007); return; }
        if (n->ops.ws_text) n->ops.ws_text(n->ud, c->id, (const char *)data, len);
    }
    /* Binary frames are rejected earlier with 1003: the protocol is JSON text. */
}

static void ws_process(JpqNet *n, Conn *c) {
    size_t off = 0;
    while (off + 2 <= c->in_len) {
        unsigned char *p = (unsigned char *)c->in + off;
        int fin = p[0] >> 7;
        int rsv = (p[0] >> 4) & 7;
        int op = p[0] & 0x0f;
        int masked = p[1] >> 7;
        uint64_t len = p[1] & 0x7f;
        size_t hdr = 2;
        if (rsv) { ws_protocol_error(n, c, 1002); return; }
        if (len == 126) {
            if (off + hdr + 2 > c->in_len) break;
            len = ((uint64_t)p[2] << 8) | p[3];
            hdr += 2;
        } else if (len == 127) {
            if (off + hdr + 8 > c->in_len) break;
            len = 0;
            for (int k = 0; k < 8; k++) len = (len << 8) | p[2 + k];
            hdr += 8;
        }
        if (op >= 8) {
            if (!fin || len > 125) { ws_protocol_error(n, c, 1002); return; }
        } else if (len > JPQ_MSG_MAX) {
            ws_protocol_error(n, c, 1009); return;
        }
        if (!masked) { ws_protocol_error(n, c, 1002); return; }
        if (off + hdr + 4 > c->in_len) break;
        unsigned char mask[4];
        memcpy(mask, c->in + off + hdr, 4);
        hdr += 4;
        if (len > SIZE_MAX - off - hdr) { ws_protocol_error(n, c, 1009); return; }
        if (off + hdr + (size_t)len > c->in_len) break;
        unsigned char *payload = (unsigned char *)c->in + off + hdr;
        for (size_t k = 0; k < (size_t)len; k++) payload[k] ^= mask[k & 3];

        if (op == 8) { /* close */
            int code = len >= 2 ? ((int)payload[0] << 8) | payload[1] : 1000;
            ws_frame(n, c, 8, payload, (size_t)len);
            conn_set_closing(n, c, code);
        } else if (op == 9) { /* ping -> pong */
            ws_frame(n, c, 10, payload, (size_t)len);
        } else if (op == 10) { /* pong */
            c->frame_op = 0;
        } else if (op == 0) { /* continuation */
            if (c->frame_op == 0) { ws_protocol_error(n, c, 1002); return; }
            if (conn_append(&c->frame, &c->frame_len, &c->frame_cap, payload, (size_t)len, JPQ_MSG_MAX)) {
                ws_protocol_error(n, c, 1009); return;
            }
            if (fin) { int fo = c->frame_op; ws_dispatch(n, c, fo, (const unsigned char *)c->frame, c->frame_len); c->frame_op = 0; c->frame_len = 0; }
        } else if (op == 1 || op == 2) {
            if (c->frame_op != 0) { ws_protocol_error(n, c, 1002); return; }
            /* the protocol is JSON text; binary data frames are unsupported */
            if (op == 2) { ws_protocol_error(n, c, 1003); return; }
            if (fin) ws_dispatch(n, c, op, payload, (size_t)len);
            else {
                c->frame_op = op;
                c->frame_len = 0;
                if (conn_append(&c->frame, &c->frame_len, &c->frame_cap, payload, (size_t)len, JPQ_MSG_MAX)) {
                    ws_protocol_error(n, c, 1009); return;
                }
            }
        } else {
            ws_protocol_error(n, c, 1002); return;
        }
        if (c->kind == CONN_FREE) return;
        off += hdr + (size_t)len;
    }
    if (off) {
        memmove(c->in, c->in + off, c->in_len - off);
        c->in_len -= off;
        c->in[c->in_len] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* HTTP parsing and dispatch                                           */
/* ------------------------------------------------------------------ */

static int header_find(const char *headers, size_t len, const char *name, char *out, size_t outsz) {
    size_t nl = strlen(name);
    size_t i = 0;
    while (i < len) {
        size_t eol = i;
        while (eol < len && headers[eol] != '\n') eol++;
        size_t lineend = eol;
        while (lineend > i && (headers[lineend - 1] == '\r' || headers[lineend - 1] == '\n')) lineend--;
        if (lineend > i && !strncasecmp(headers + i, name, nl) && headers[i + nl] == ':') {
            size_t v = i + nl + 1;
            while (v < lineend && (headers[v] == ' ' || headers[v] == '\t')) v++;
            size_t n = lineend - v;
            if (n >= outsz) n = outsz - 1;
            memcpy(out, headers + v, n);
            out[n] = 0;
            return 1;
        }
        if (eol >= len) break;
        i = eol + 1;
    }
    return 0;
}

static const char *http_reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 501: return "Not Implemented";
    default: return "Response";
    }
}

static void http_send(JpqNet *n, Conn *c, int status, const char *ctype, const char *body, size_t len) {
    struct sb r = {0};
    sb_fmt(&r, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
              "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
           status, http_reason(status), ctype ? ctype : "application/octet-stream", len);
    if (r.p) conn_queue(n, c, r.p, r.len);
    free(r.p);
    if (len) conn_queue(n, c, body, len);
    conn_set_closing(n, c, 0);
    conn_flush(n, c);
}

static void http_dispatch(JpqNet *n, Conn *c) {
    if (c->dispatched) return;
    size_t hdr_end = 0;
    for (size_t i = 0; i + 3 < c->in_len; i++)
        if (c->in[i] == '\r' && c->in[i + 1] == '\n' && c->in[i + 2] == '\r' && c->in[i + 3] == '\n') { hdr_end = i + 4; break; }
    if (!hdr_end) {
        /* wait for the rest of the header, but never let it grow unbounded */
        if (c->in_len >= JPQ_HEADER_MAX) http_send(n, c, 431, "application/json", "{\"ok\":false}", 11);
        return;
    }

    JpqHttpRequest req;
    memset(&req, 0, sizeof req);
    req.conn = c->id;
    snprintf(req.ip, sizeof req.ip, "%s", c->ip);
    char version[16];
    char extra;
    int matched = sscanf(c->in, "%7s %1023s %15s %c", req.method, req.path, version, &extra);
    if (matched < 3 || (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0")) || req.path[0] != '/') {
        http_send(n, c, 400, "application/json", "{\"ok\":false}", 11);
        return;
    }
    char *q = strchr(req.path, '?');
    if (q) { snprintf(req.query, sizeof req.query, "%s", q + 1); *q = 0; }

    char clen[32];
    size_t body_len = 0;
    if (header_find(c->in, hdr_end, "Content-Length", clen, sizeof clen)) {
        char *endp = NULL;
        unsigned long v = strtoul(clen, &endp, 10);
        if (!*clen || (endp && *endp) || v > JPQ_BODY_MAX) { http_send(n, c, 413, "application/json", "{\"ok\":false}", 11); return; }
        body_len = (size_t)v;
    }
    if (c->in_len < hdr_end + body_len) return; /* wait for the body */
    c->body_len = body_len;
    c->dispatched = 1;

    char *body = malloc(body_len + 1);
    if (!body) { http_send(n, c, 500, "application/json", "{\"ok\":false}", 11); return; }
    if (body_len) memcpy(body, c->in + hdr_end, body_len);
    body[body_len] = 0;
    req.body = body;
    req.body_len = body_len;

    int ws = 0;
    if (!strcmp(req.method, "GET") && n->ops.ws_path && n->ops.ws_path(n->ud, &req)) {
        char upgrade[32], key[128];
        if (header_find(c->in, hdr_end, "Upgrade", upgrade, sizeof upgrade) &&
            !strcasecmp(upgrade, "websocket") &&
            header_find(c->in, hdr_end, "Sec-WebSocket-Key", key, sizeof key)) {
            char concat[256];
            snprintf(concat, sizeof concat, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
            unsigned char digest[20];
            sha1((const unsigned char *)concat, strlen(concat), digest);
            char accept[64];
            b64_encode(digest, 20, accept, sizeof accept);
            struct sb r = {0};
            sb_fmt(&r, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                       "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
            if (r.p) conn_queue(n, c, r.p, r.len);
            free(r.p);
            c->is_ws = 1;
            c->last_rx = c->last_ping = ms_now();
            ws = 1;
            if (n->ops.ws_open) n->ops.ws_open(n->ud, c->id);
        }
    }
    if (!ws) {
        size_t outlen = 0;
        const char *ctype = "application/json";
        int status = 200;
        char *resp = n->ops.http ? n->ops.http(n->ud, &req, &outlen, &ctype, &status) : NULL;
        if (!resp) { outlen = 0; }
        http_send(n, c, status, ctype, resp ? resp : "", outlen);
        free(resp);
    }
    free(body);

    size_t consumed = hdr_end + body_len;
    if (consumed < c->in_len) {
        memmove(c->in, c->in + consumed, c->in_len - consumed);
        c->in_len -= consumed;
        c->in[c->in_len] = 0;
    } else {
        c->in_len = 0;
        if (c->in) c->in[0] = 0;
    }
    if (ws && c->frame_op == 0 && c->in_len) ws_process(n, c);
}

static void conn_read(JpqNet *n, Conn *c) {
    char tmp[8192];
    for (;;) {
        ssize_t r = read(c->fd, tmp, sizeof tmp);
        if (r > 0) {
            if (conn_append(&c->in, &c->in_len, &c->in_cap, tmp, (size_t)r,
                            c->kind == CONN_LINE ? JPQ_LINE_MAX + 1 : (c->is_ws ? JPQ_IN_MAX : JPQ_HEADER_MAX + JPQ_BODY_MAX))) {
                /* line overflow is discarded by the line parser; others close */
                if (c->kind == CONN_LINE) { c->in_len = 0; }
                else { ws_protocol_error(n, c, 1009); return; }
            }
            c->last_rx = ms_now();
            if (c->is_ws) ws_process(n, c);
            else if (c->kind == CONN_HTTP) http_dispatch(n, c);
            else line_drain(n, c);
            if (c->kind == CONN_FREE) return;
            if (c->closing) return;
        } else if (r == 0) {
            conn_close_now(n, c);
            return;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            conn_close_now(n, c);
            return;
        }
    }
}

static void line_drain(JpqNet *n, Conn *c) {
    size_t start = 0;
    for (size_t i = 0; i < c->in_len; i++) {
        if (c->in[i] == '\n') {
            size_t end = i;
            if (end > start && c->in[end - 1] == '\r') end--;
            size_t len = end - start;
            if (len > 0 && n->ops.line) n->ops.line(n->ud, c->id, c->in + start, len);
            start = i + 1;
        }
    }
    if (start) {
        memmove(c->in, c->in + start, c->in_len - start);
        c->in_len -= start;
        c->in[c->in_len] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Accept / lifecycle                                                  */
/* ------------------------------------------------------------------ */

static Conn *conn_alloc(JpqNet *n, int fd, int kind) {
    for (int i = 0; i < JPQ_NET_MAX_CONN; i++) {
        Conn *c = &n->conns[i];
        if (c->kind == CONN_FREE) {
            memset(c, 0, sizeof *c);
            c->fd = fd;
            c->kind = kind;
            c->id = ++n->next_id;
            if (!c->id) c->id = ++n->next_id;
            c->last_rx = c->last_ping = ms_now();
            return c;
        }
    }
    return NULL;
}

static int count_kind(JpqNet *n, int kind) {
    int k = 0;
    for (int i = 0; i < JPQ_NET_MAX_CONN; i++) if (n->conns[i].kind == kind) k++;
    return k;
}

static void accept_ready(JpqNet *n, int lfd, int kind, int max) {
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        int fd = accept4(lfd, (struct sockaddr *)&ss, &sl, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (count_kind(n, kind) >= max) { close(fd); continue; }
        Conn *c = conn_alloc(n, fd, kind);
        if (!c) { close(fd); continue; }
        if (ss.ss_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
            inet_ntop(AF_INET, &sin->sin_addr, c->ip, sizeof c->ip);
        } else {
            snprintf(c->ip, sizeof c->ip, "local");
        }
    }
}

static void heartbeat(JpqNet *n, uint64_t now) {
    (void)now;
    uint64_t ts = ms_now();
    for (int i = 0; i < JPQ_NET_MAX_CONN; i++) {
        Conn *c = &n->conns[i];
        if (c->kind == CONN_FREE) continue;
        if (ts > c->last_rx && ts - c->last_rx > JPQ_IDLE_MS) { conn_set_closing(n, c, 1001); continue; }
        if (c->kind == CONN_HTTP && c->is_ws && ts - c->last_ping >= JPQ_PING_MS) {
            ws_frame(n, c, 9, NULL, 0);
            c->last_ping = ts;
        }
    }
}

JpqNet *jpq_net_create(const JpqNetOps *ops, void *ud) {
    JpqNet *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->ops = *ops;
    n->ud = ud;
    n->tcp_fd = n->unix_fd = -1;
    for (int i = 0; i < JPQ_NET_MAX_CONN; i++) n->conns[i].fd = -1;
    return n;
}

int jpq_net_listen_tcp(JpqNet *n, const char *host, int port, int *port_out) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (!host || !*host || !strcmp(host, "0.0.0.0")) a.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { close(fd); return -1; }
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 16)) { close(fd); return -1; }
    struct sockaddr_in bound;
    socklen_t bl = sizeof bound;
    if (getsockname(fd, (struct sockaddr *)&bound, &bl)) { close(fd); return -1; }
    n->port = ntohs(bound.sin_port);
    n->tcp_fd = fd;
    if (port_out) *port_out = n->port;
    return 0;
}

int jpq_net_listen_unix_lines(JpqNet *n, const char *path) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (!path || strlen(path) >= sizeof a.sun_path) return -1;
    strcpy(a.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    unlink(path);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || chmod(path, 0600) || listen(fd, 4)) { close(fd); return -1; }
    n->unix_fd = fd;
    return 0;
}

int jpq_net_ws_send(JpqNet *n, JpqConnId conn, const char *data, size_t len) {
    Conn *c = conn_by_id(n, conn);
    if (!c || !c->is_ws) return -1;
    if (ws_frame(n, c, 1, data, len)) return -1;
    conn_flush(n, c);
    return 0;
}

int jpq_net_line_send(JpqNet *n, JpqConnId conn, const char *data, size_t len) {
    Conn *c = conn_by_id(n, conn);
    if (!c || c->kind != CONN_LINE || c->closing) return -1;
    if (conn_queue(n, c, data, len) || conn_queue(n, c, "\n", 1)) return -1;
    conn_flush(n, c);
    return 0;
}

void jpq_net_conn_close(JpqNet *n, JpqConnId conn, int code) {
    Conn *c = conn_by_id(n, conn);
    if (!c) return;
    if (c->is_ws) {
        unsigned char body[2] = { (unsigned char)(code >> 8), (unsigned char)(code & 0xff) };
        ws_frame(n, c, 8, body, 2);
    }
    conn_set_closing(n, c, code);
    conn_flush(n, c);
}

int jpq_net_bound_port(JpqNet *n) { return n->port; }

void jpq_net_stop(JpqNet *n) { n->stop = 1; }

void jpq_net_run(JpqNet *n) {
    while (!n->stop) {
        struct pollfd pfds[4 + JPQ_NET_MAX_CONN];
        int map[4 + JPQ_NET_MAX_CONN];
        int nf = 0;
        if (n->tcp_fd >= 0) { pfds[nf].fd = n->tcp_fd; pfds[nf].events = POLLIN; pfds[nf].revents = 0; map[nf] = -1; nf++; }
        if (n->unix_fd >= 0) { pfds[nf].fd = n->unix_fd; pfds[nf].events = POLLIN; pfds[nf].revents = 0; map[nf] = -2; nf++; }
        for (int i = 0; i < JPQ_NET_MAX_CONN; i++) {
            Conn *c = &n->conns[i];
            if (c->kind == CONN_FREE) continue;
            pfds[nf].fd = c->fd;
            pfds[nf].events = POLLIN | (c->out_off < c->out_len ? POLLOUT : 0);
            pfds[nf].revents = 0;
            map[nf] = i;
            nf++;
        }
        int rc = poll(pfds, (nfds_t)nf, 50);
        uint64_t now = ms_now();
        if (rc > 0) {
            for (int i = 0; i < nf; i++) {
                if (!pfds[i].revents) continue;
                if (map[i] == -1) { accept_ready(n, n->tcp_fd, CONN_HTTP, JPQ_NET_MAX_HTTP); continue; }
                if (map[i] == -2) { accept_ready(n, n->unix_fd, CONN_LINE, JPQ_NET_MAX_LINE); continue; }
                Conn *c = &n->conns[map[i]];
                if (c->kind == CONN_FREE) continue;
                if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { conn_close_now(n, c); continue; }
                if (pfds[i].revents & POLLOUT) { if (conn_flush(n, c)) { conn_close_now(n, c); continue; } }
                if (pfds[i].revents & POLLIN) {
                    conn_read(n, c);
                    if (c->kind == CONN_FREE) continue;
                }
                if (c->closing && c->out_off >= c->out_len) conn_close_now(n, c);
            }
        }
        heartbeat(n, now);
        if (n->ops.tick) n->ops.tick(n->ud, now);
        conn_reap(n);
    }
}

void jpq_net_destroy(JpqNet *n) {
    if (!n) return;
    for (int i = 0; i < JPQ_NET_MAX_CONN; i++) conn_close_now(n, &n->conns[i]);
    if (n->tcp_fd >= 0) close(n->tcp_fd);
    if (n->unix_fd >= 0) close(n->unix_fd);
    free(n);
}
