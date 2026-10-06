/* Frigate integration: only names/codecs leave the backend. Config may
 * contain camera credentials; never forward its body to the TV or logs. */
#ifndef FRIGATE_HOST
#define FRIGATE_HOST "192.168.88.39"
#endif
#ifndef FRIGATE_PORT
#define FRIGATE_PORT 5000
#endif
#ifndef REBOX_FRIGATE_MODULE
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
#endif
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
        for (size_t i = 0; i < cams->n && i < 128; i++) {
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
#ifndef REBOX_FRIGATE_MODULE
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

#endif
