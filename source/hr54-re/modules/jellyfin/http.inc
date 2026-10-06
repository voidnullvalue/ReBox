/* Extracted Jellyfin client, auth, browsing and PlaybackInfo implementation. */
struct jf_resp {
    int status;
    char ctype[128];
    char *body; /* single allocation; header text precedes body in it */
    size_t body_len;
};

static int tcp_connect(const char *host, int port, int timeout_s) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {timeout_s, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr)) {
        close(fd);
        return -1;
    }
    return fd;
}

static void resp_free(struct jf_resp *r) {
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
}

/* Locate "\r\n\r\n"; fill status and Content-Type; return header length. */
static int http_split(char *buf, size_t len, struct jf_resp *out, size_t *hdr_len) {
    buf[len] = 0;
    char *split = strstr(buf, "\r\n\r\n");
    if (!split) return -1;
    *hdr_len = (size_t)(split - buf) + 4;
    out->status = 0;
    if (!strncmp(buf, "HTTP/", 5)) {
        char *sp = strchr(buf, ' ');
        if (sp) out->status = atoi(sp + 1);
    }
    out->ctype[0] = 0;
    char *line = buf;
    while (line < split) {
        char *eol = strstr(line, "\r\n");
        if (!eol || eol > split) eol = split;
        if (!strncasecmp(line, "Content-Type:", 13)) {
            char *v = line + 13;
            while (v < eol && *v == ' ') v++;
            size_t n = (size_t)(eol - v);
            if (n >= sizeof out->ctype) n = sizeof out->ctype - 1;
            memcpy(out->ctype, v, n);
            out->ctype[n] = 0;
        }
        line = eol + 2;
    }
    return 0;
}

/* Blocking request; the whole response is buffered up to cap.  0 on reply. */
static int jf_http(const char *host, int port, const char *method,
                   const char *pathq, const char *token, const char *authz,
                   const char *body, struct jf_resp *out, size_t cap) {
    memset(out, 0, sizeof *out);
    int fd = tcp_connect(host, port, 30);
    if (fd < 0) return fail("Jellyfin connection failed");
    struct sb req = {0};
    sb_fmt(&req, "%s %s%s HTTP/1.0\r\nHost: %s:%d\r\nAccept: application/json\r\n",
           method, *pathq == '/' ? "" : "/", pathq, host, port);
    if (token && *token) sb_fmt(&req, "X-Emby-Token: %s\r\n", token);
    if (authz && *authz) sb_fmt(&req, "Authorization: %s\r\n", authz);
    if (body) sb_fmt(&req, "Content-Type: application/json\r\nContent-Length: %zu\r\n",
                     strlen(body));
    sb_puts(&req, "Connection: close\r\n\r\n");
    if (body) sb_puts(&req, body);
    int rc = -1;
    if (!req.p) goto done;
    if (write_all_fd(fd, req.p, req.len)) goto done;
    struct sb resp = {0};
    for (;;) {
        char chunk[16384];
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        if (resp.len + (size_t)n > cap) { free(resp.p); goto done; }
        if (sb_putn(&resp, chunk, (size_t)n)) { free(resp.p); goto done; }
    }
    if (!resp.p) goto done;
    {
        size_t hdr = 0;
        if (http_split(resp.p, resp.len, out, &hdr)) { free(resp.p); goto done; }
        out->body = resp.p; /* owns the single allocation */
        out->body_len = resp.len - hdr;
        memmove(out->body, resp.p + hdr, out->body_len);
        out->body[out->body_len] = 0;
    }
    rc = 0;
done:
    free(req.p);
    close(fd);
    return rc;
}

/* Streaming open for the /play relay: returns fd with headers consumed and
 * any body bytes already read available in pre[]. */
struct jf_stream {
    int fd;
    char pre[RELAY_CHUNK];
    size_t pre_len;
    int status;
};

static int jf_stream_open(const char *host, int port, const char *pathq,
                          const char *token, struct jf_stream *out) {
    memset(out, 0, sizeof *out);
    int fd = tcp_connect(host, port, 120);
    if (fd < 0) return -1;
    struct sb req = {0};
    sb_fmt(&req, "GET %s%s HTTP/1.0\r\nHost: %s:%d\r\nAccept: */*\r\n",
           *pathq == '/' ? "" : "/", pathq, host, port);
    if (token && *token) sb_fmt(&req, "X-Emby-Token: %s\r\n", token);
    sb_puts(&req, "Connection: close\r\n\r\n");
    if (!req.p || write_all_fd(fd, req.p, req.len)) {
        free(req.p);
        close(fd);
        return -1;
    }
    free(req.p);
    size_t have = 0;
    for (;;) {
        ssize_t n = read(fd, out->pre + have, sizeof out->pre - have);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        have += (size_t)n;
        out->pre[have] = 0;
        if (strstr(out->pre, "\r\n\r\n")) break;
        if (have >= sizeof out->pre - 1) break;
    }
    if (have < 4) { close(fd); return -1; }
    out->pre[have] = 0;
    char *split = strstr(out->pre, "\r\n\r\n");
    if (!split) { close(fd); return -1; }
    size_t hdr = (size_t)(split - out->pre) + 4;
    out->status = 0;
    if (!strncmp(out->pre, "HTTP/", 5)) {
        char *sp = strchr(out->pre, ' ');
        if (sp) out->status = atoi(sp + 1);
    }
    memmove(out->pre, out->pre + hdr, have - hdr);
    out->pre_len = have - hdr;
    out->fd = fd;
    return 0;
}


