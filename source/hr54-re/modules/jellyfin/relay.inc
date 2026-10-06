static void jellyfin_relay_finished(const char *token) {
    int ended = 0, return_to_tv = 0;
    state_lock();
    struct stream_slot *s = slot_by_token(token);
    if (s) s->closed = 1;
    if (S->playing && !S->play_live && !strcmp(S->play_token, token)) {
        return_to_tv = S->return_to_tv;
        playback_end_locked();
        ended = 1;
    }
    state_unlock();
    /* The relay's own session ended (stop/seek/EOF): report it so Jellyfin
     * kills the ffmpeg job instead of encoding to EOF. */
    flush_stop_report();
    if (ended) schedule_tv_return(return_to_tv, POST_STOP_DISMISS);
}

/* EOF means all bytes were delivered, not that the decoder presented them.
 * Keep session ownership while the receiver consumes buffered media. Runtime
 * comes from the already-required Jellyfin item lookup, not another state store.
 * The allowance covers playURL/decoder startup; it is deliberately conservative.
 */
#ifndef JF_DECODER_STARTUP_GRACE
#define JF_DECODER_STARTUP_GRACE 15.0
#endif
static double jellyfin_drain_remaining_locked(const char *token, double eof_at) {
    struct stream_slot *s = slot_by_token(token);
    if (!s || s->closed || !S->playing || S->play_live || strcmp(S->play_token, token)) return 0;
    double remaining = S->play_duration > 0
        ? S->play_duration + JF_DECODER_STARTUP_GRACE - playback_elapsed_locked()
        : JF_DECODER_STARTUP_GRACE - (mono_now() - eof_at);
    return remaining > 0 ? remaining : 0;
}
static void jellyfin_drain(const char *token) {
    double eof_at = mono_now();
    state_lock();
    struct stream_slot *s = slot_by_token(token);
    if (s && !s->closed) s->draining = 1;
    state_unlock();
    jf_log("Jellyfin upstream EOF; allowing buffered decoder playback to drain");
    /* A small title can finish downloading before start_play commits its state. */
    for (;;) {
        state_lock();
        s = slot_by_token(token);
        int pending = s && !s->closed && !S->playing && mono_now() - eof_at < CLAIM_WAIT;
        double remaining = jellyfin_drain_remaining_locked(token, eof_at);
        state_unlock();
        if (!pending && remaining <= 0) break;
        nap(0.1);
    }
}

static void serve_play(int fd, const char *token_raw) {
    char token[64];
    snprintf(token, sizeof token, "%.60s", token_raw);
    char *dot = strrchr(token, '.');
    if (dot && !strcmp(dot, ".ts")) *dot = 0;
    if (!is_alnum_str(token)) {
        send_json_error(fd, 404, "unknown stream", "GET");
        return;
    }
    alarm(0); /* a movie relay must not die to the per-request alarm */
    state_lock();
    struct stream_slot *s = slot_by_token(token);
    if (!s || s->claimed || s->closed) {
        state_unlock();
        send_json_error(fd, 404, "unknown stream", "GET");
        return;
    }
    s->claimed = 1;
    char upath[JF_URL_MAX];
    snprintf(upath, sizeof upath, "%s", s->upath);
    state_unlock();
    jf_log("receiver opened stream");
    struct jf_stream st;
    if (jf_stream_open(S->jf_host, S->jf_port, upath, S->token, &st)) {
        jf_log("upstream stream unavailable");
        jellyfin_relay_finished(token);
        send_json_error(fd, 502, "Jellyfin stream unavailable", "GET");
        return;
    }
    if (st.status != 200) {
        jf_log("Jellyfin stream returned HTTP %d", st.status);
        close(st.fd);
        jellyfin_relay_finished(token);
        send_json_error(fd, 502, "Jellyfin rejected the media stream", "GET");
        return;
    }
    if (!st.pre_len) {
        ssize_t n;
        do { n = read(st.fd, st.pre, sizeof st.pre); } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            jf_log("Jellyfin stream ended without media bytes");
            close(st.fd);
            jellyfin_relay_finished(token);
            send_json_error(fd, 502, "Jellyfin stream contained no media", "GET");
            return;
        }
        st.pre_len = (size_t)n;
    }
    if ((unsigned char)st.pre[0] != 0x47) {
        jf_log("Jellyfin response was not MPEG-TS (first byte %02x)", (unsigned char)st.pre[0]);
        close(st.fd);
        jellyfin_relay_finished(token);
        send_json_error(fd, 502, "Jellyfin returned invalid MPEG-TS media", "GET");
        return;
    }
    jf_log("Jellyfin relay ready: HTTP %d, initial bytes %zu", st.status, st.pre_len);
    state_lock();
    s = slot_by_token(token);
    if (s && !s->closed) s->ready = 1;
    state_unlock();
    char head[256];
    int hn = snprintf(head, sizeof head,
        "HTTP/1.0 200 OK\r\nContent-Type: video/mp2t\r\nConnection: close\r\n\r\n");
    if ((hn > 0 && write_all_fd(fd, head, (size_t)hn)) ||
        (st.pre_len && write_all_fd(fd, st.pre, st.pre_len))) {
        close(st.fd); jellyfin_relay_finished(token); return;
    }
    char *buf = malloc(RELAY_CHUNK);
    if (!buf) { close(st.fd); jellyfin_relay_finished(token); return; }
    int clean_eof = 0;
    for (;;) {
        int closed = 0, paused = 0;
        state_lock();
        struct stream_slot *gate = slot_by_token(token);
        closed = !gate || gate->closed;
        paused = gate && gate->paused && !closed;
        state_unlock();
        if (closed) { jf_log("stream closed by control"); break; }
        if (paused) { nap(0.2); continue; }
        ssize_t n = read(st.fd, buf, RELAY_CHUNK);
        if (n < 0) {
            if (errno == EINTR) continue;
            jf_log("upstream read error");
            break;
        }
        if (n == 0) { clean_eof = 1; break; }
        if (write_all_fd(fd, buf, (size_t)n)) {
            jf_log("receiver closed stream");
            break;
        }
    }
    free(buf);
    close(st.fd);
    if (clean_eof) {
        shutdown(fd, SHUT_WR); /* Deliver HTTP EOF so the decoder can flush. */
        jellyfin_drain(token);
    }
    jellyfin_relay_finished(token);
}

