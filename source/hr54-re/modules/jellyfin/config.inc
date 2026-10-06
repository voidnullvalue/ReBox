static void config_paths(char *dir, size_t dsz, char *cfg, size_t csz,
                         char *tok, size_t tsz) {
    snprintf(dir, dsz, "%s/config", persist_root);
    snprintf(cfg, csz, "%s/config.json", dir);
    snprintf(tok, tsz, "%s/token", dir);
}

/* Caller holds the state lock. */
static int save_config_locked(void) {
    char dir[512], cfg[576], tok[640];
    config_paths(dir, sizeof dir, cfg, sizeof cfg, tok, sizeof tok);
    struct sb b = {0};
    sb_puts(&b, "{\"server\":\"http://");
    sb_puts(&b, S->jf_host);
    sb_fmt(&b, ":%d\",\"device_id\":\"%s\"", S->jf_port, S->device_id);
    if (S->user_id[0]) sb_fmt(&b, ",\"user_id\":\"%s\"", S->user_id);
    if (S->user_name[0]) {
        sb_puts(&b, ",\"user_name\":");
        sb_json_str(&b, S->user_name);
    }
    sb_puts(&b, "}\n");
    int rc = b.p ? atomic_write(cfg, b.p, b.len, 0600) : -1;
    free(b.p);
    return rc;
}

/* Caller holds the state lock. */
static int save_token_locked(const char *token) {
    char dir[512], cfg[576], tok[640];
    config_paths(dir, sizeof dir, cfg, sizeof cfg, tok, sizeof tok);
    if (!token || !*token) { unlink(tok); return 0; }
    char data[600];
    int n = snprintf(data, sizeof data, "%s\n", token);
    if (n < 0 || (size_t)n >= sizeof data) return -1;
    return atomic_write(tok, data, (size_t)n, 0600);
}

/* One policy table, independent of authentication and browse checkpoints. */
static const struct { const char *name; int bitrate; } jf_qualities[] = {
    {"Conservative", 8000000}, {"High", 12000000}, {"Maximum", 16000000}
};
#define JF_QUALITY_DEFAULT 1
static int jf_quality_index(double bitrate) {
    for (size_t i = 0; i < sizeof jf_qualities / sizeof jf_qualities[0]; i++)
        if (bitrate == jf_qualities[i].bitrate) return (int)i;
    return -1;
}
static int jf_quality_safe(double bitrate) {
    int i = jf_quality_index(bitrate);
    return jf_qualities[i < 0 ? JF_QUALITY_DEFAULT : i].bitrate;
}
static void jf_quality_path(char *path, size_t sz) {
    snprintf(path, sz, "%s/config/playback-quality.json", persist_root);
}
static void jf_quality_load(void) {
    S->jf_quality_bitrate = jf_quality_safe(0);
    char path[640]; size_t n;
    jf_quality_path(path, sizeof path);
    char *raw = read_file(path, &n);
    if (raw) {
        struct jval *v = json_parse(raw, n);
        S->jf_quality_bitrate = jf_quality_safe(jnum(jget(v, "jellyfinVideoBitrate"), 0));
        jfree(v); free(raw);
    }
}
/* Caller holds the shared lock, including the atomic persistence operation. */
static void jf_settings_locked(struct sb *out) {
    sb_fmt(out, "{\"configured\":%s,\"authenticated\":%s,\"writable\":true,"
        "\"settings\":{\"jellyfinVideoBitrate\":%d},"
        "\"writableKeys\":[\"jellyfinVideoBitrate\"],\"qualityLevels\":[",
        S->jf_host[0] ? "true" : "false", S->auth_valid ? "true" : "false",
        jf_quality_safe(S->jf_quality_bitrate));
    for (size_t i = 0; i < sizeof jf_qualities / sizeof jf_qualities[0]; i++) {
        if (i) sb_puts(out, ",");
        sb_fmt(out, "{\"name\":\"%s\",\"bitrate\":%d}", jf_qualities[i].name, jf_qualities[i].bitrate);
    }
    sb_puts(out, "]}");
}
static int jf_quality_save_locked(int bitrate) {
    char path[640], raw[100];
    jf_quality_path(path, sizeof path);
    int n = snprintf(raw, sizeof raw, "{\"jellyfinVideoBitrate\":%d}\n", bitrate);
    if (atomic_write(path, raw, (size_t)n, 0600)) return fail("could not save Jellyfin video quality");
    S->jf_quality_bitrate = bitrate;
    return 0;
}

/* Run once in the parent before forking; no lock needed. */
static void load_persist(void) {
    char dir[512], cfg[576], tok[640];
    config_paths(dir, sizeof dir, cfg, sizeof cfg, tok, sizeof tok);
    jf_quality_load();
    size_t n = 0;
    char *raw = read_file(cfg, &n);
    if (raw) {
        struct jval *v = json_parse(raw, n);
        if (v) {
            const char *did = jstr(jget(v, "device_id"));
            if (did && strlen(did) < sizeof S->device_id)
                snprintf(S->device_id, sizeof S->device_id, "%s", did);
            jfree(v);
        }
        free(raw);
    }
    if (!S->device_id[0]) {
        if (!random_hex(S->device_id, 32)) save_config_locked();
        else jf_log("cannot read /dev/urandom for device id");
    }
    raw = read_file(tok, &n);
    if (raw) {
        while (n && (raw[n - 1] == '\n' || raw[n - 1] == '\r' || raw[n - 1] == ' '))
            raw[--n] = 0;
        if (n && n < sizeof S->token) {
            memcpy(S->token, raw, n);
            S->token[n] = 0;
        }
        free(raw);
    }
}


