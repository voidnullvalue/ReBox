static int require_token(void) {
    if (!S->token[0]) return fail("sign in to Jellyfin first");
    return 0;
}

static void media_browser_auth(struct sb *out) {
    sb_fmt(out, "MediaBrowser Client=\"HR54 Jellyfin\", Device=\"DIRECTV HR54\", "
                "DeviceId=\"%s\", Version=\"0.1\"", S->device_id);
    if (S->token[0]) sb_fmt(out, ", Token=\"%s\"", S->token);
}

static int jf_get(const char *pathq, struct jf_resp *r, size_t cap) {
    return jf_http(S->jf_host, S->jf_port, "GET", pathq, S->token, NULL, NULL, r, cap);
}

static int jf_post(const char *pathq, const char *body, struct jf_resp *r, size_t cap) {
    return jf_http(S->jf_host, S->jf_port, "POST", pathq, S->token, NULL, body, r, cap);
}

/* Tell Jellyfin a playback session ended. Without this the server keeps the
 * ffmpeg job alive until it finishes encoding the whole title at 2.2x speed,
 * leaving multi-GB .ts files in its transcode cache until the 24h cleanup
 * task runs — which is how the system disk filled. */
static void jf_report_stopped(const char *session) {
    if (!session || !session[0] || !S->token[0]) return;
    struct sb body = {0};
    sb_fmt(&body, "{\"PlaySessionId\":\"%.63s\"}", session);
    struct jf_resp r;
    if (jf_post("Sessions/Playing/Stopped", body.p, &r, 16 * 1024))
        jf_log("playback stop report failed: %s", g_err);
    else if (r.status >= 300)
        jf_log("playback stop report: HTTP %d", r.status);
    else
        jf_log("reported playback stopped (session %.24s...)", session);
    resp_free(&r);
    free(body.p);
}

/* Swap out a pending session id and report it. Lock is not held on entry. */
static void flush_stop_report(void) {
    char session[64];
    state_lock();
    snprintf(session, sizeof session, "%.63s", S->stop_pending);
    S->stop_pending[0] = 0;
    state_unlock();
    jf_report_stopped(session);
}

/* Validate the token via Users/Me; on failure clear in-memory auth. */
static int jf_validate_token(void) {
    struct jf_resp r;
    if (jf_get("Users/Me", &r, 64 * 1024)) {
        state_lock();
        S->token[0] = 0;
        S->user_id[0] = 0;
        S->user_name[0] = 0;
        S->auth_valid = 0;
        state_unlock();
        return -1;
    }
    int ok = 0;
    if (r.status == 200 && r.body) {
        struct jval *v = json_parse(r.body, r.body_len);
        if (v) {
            const char *id = jstr(jget(v, "Id"));
            const char *name = jstr(jget(v, "Name"));
            if (id && strlen(id) < sizeof S->user_id) {
                state_lock();
                snprintf(S->user_id, sizeof S->user_id, "%s", id);
                if (name) snprintf(S->user_name, sizeof S->user_name, "%.200s", name);
                S->auth_valid = 1;
                state_unlock();
                ok = 1;
            }
            jfree(v);
        }
    }
    resp_free(&r);
    if (!ok) {
        state_lock();
        S->token[0] = 0;
        S->user_id[0] = 0;
        S->user_name[0] = 0;
        S->auth_valid = 0;
        state_unlock();
    }
    return ok ? 0 : -1;
}

static void qc_status_json(struct sb *out, int authenticated, int pending,
                           const char *user, const char *code, int expired) {
    sb_puts(out, "{\"authenticated\":");
    sb_puts(out, authenticated ? "true" : "false");
    sb_puts(out, ",\"user\":");
    sb_json_str(out, user ? user : "");
    sb_puts(out, ",\"pending\":");
    sb_puts(out, pending ? "true" : "false");
    sb_puts(out, ",\"code\":");
    if (code && *code) sb_json_str(out, code);
    else sb_puts(out, "null");
    if (expired) sb_puts(out, ",\"expired\":true");
    sb_puts(out, "}");
}

/* Caller holds the state lock. */
static void auth_status_locked(struct sb *out) {
    qc_status_json(out, S->auth_valid, !!S->qc_secret[0], S->user_name, S->qc_code, 0);
}

static void qc_clear_locked(void) {
    S->qc_secret[0] = 0;
    S->qc_code[0] = 0;
    S->qc_started = 0;
}

/* POST /api/auth/start */
static int qc_start(struct sb *out) {
    char authz[640];
    struct sb ab = {0};
    media_browser_auth(&ab);
    snprintf(authz, sizeof authz, "%s", ab.p ? ab.p : "");
    free(ab.p);
    struct jf_resp r;
    if (jf_http(S->jf_host, S->jf_port, "GET", "QuickConnect/Enabled", NULL, authz,
                NULL, &r, 4096))
        return fail("Quick Connect availability check failed");
    int enabled = 0;
    if (r.status == 200 && r.body) {
        struct jval *v = json_parse(r.body, r.body_len);
        enabled = v && v->t == J_TRUE;
        jfree(v);
    }
    resp_free(&r);
    if (!enabled) return fail("Jellyfin Quick Connect is disabled");
    if (jf_http(S->jf_host, S->jf_port, "POST", "QuickConnect/Initiate", NULL, authz,
                "{}", &r, 64 * 1024))
        return fail("Quick Connect initiate failed");
    char secret[512] = "", code[16] = "";
    if (r.status == 200 && r.body) {
        struct jval *v = json_parse(r.body, r.body_len);
        if (v) {
            const char *sec = jstr(jget(v, "Secret"));
            const char *c = jstr(jget(v, "Code"));
            if (sec && *sec && c && *c) {
                snprintf(secret, sizeof secret, "%.500s", sec);
                snprintf(code, sizeof code, "%.15s", c);
            }
            jfree(v);
        }
    }
    resp_free(&r);
    if (!secret[0] || !code[0])
        return fail("Quick Connect response lacks secret or code");
    state_lock();
    snprintf(S->qc_secret, sizeof S->qc_secret, "%s", secret);
    snprintf(S->qc_code, sizeof S->qc_code, "%s", code);
    S->qc_started = mono_now();
    state_unlock();
    sb_puts(out, "{\"code\":");
    sb_json_str(out, code);
    sb_puts(out, ",\"pending\":true}");
    return 0;
}

/* GET /api/auth/poll */
static int qc_poll(struct sb *out) {
    char secret[512];
    double started;
    state_lock();
    int pending = !!S->qc_secret[0];
    snprintf(secret, sizeof secret, "%s", S->qc_secret);
    started = S->qc_started;
    if (!pending) auth_status_locked(out);
    state_unlock();
    if (!pending) return 0;
    if (mono_now() - started > QC_LIFETIME) {
        state_lock();
        qc_clear_locked();
        state_unlock();
        qc_status_json(out, 0, 0, "", NULL, 1);
        return 0;
    }
    char authz[640];
    struct sb ab = {0};
    media_browser_auth(&ab);
    snprintf(authz, sizeof authz, "%s", ab.p ? ab.p : "");
    free(ab.p);
    struct sb path = {0};
    sb_puts(&path, "QuickConnect/Connect?Secret=");
    url_encode(&path, secret);
    struct jf_resp r;
    if (jf_http(S->jf_host, S->jf_port, "GET", path.p ? path.p : "", NULL, authz,
                NULL, &r, 64 * 1024)) {
        free(path.p);
        return fail("Quick Connect status check failed");
    }
    free(path.p);
    int authenticated = 0;
    if (r.status == 200 && r.body) {
        struct jval *v = json_parse(r.body, r.body_len);
        if (v) authenticated = jbool(jget(v, "Authenticated"), 0);
        jfree(v);
    }
    resp_free(&r);
    if (!authenticated) {
        state_lock();
        auth_status_locked(out);
        state_unlock();
        return 0;
    }
    struct sb body = {0};
    sb_puts(&body, "{\"Secret\":");
    sb_json_str(&body, secret);
    sb_puts(&body, "}");
    if (jf_http(S->jf_host, S->jf_port, "POST", "Users/AuthenticateWithQuickConnect",
                NULL, authz, body.p, &r, 256 * 1024)) {
        free(body.p);
        return fail("Quick Connect login failed");
    }
    free(body.p);
    char token[512] = "", user_id[JF_ID_MAX] = "", user_name[JF_NAME_MAX] = "";
    if (r.status == 200 && r.body) {
        struct jval *v = json_parse(r.body, r.body_len);
        if (v) {
            const char *t = jstr(jget(v, "AccessToken"));
            struct jval *u = jget(v, "User");
            const char *uid = u ? jstr(jget(u, "Id")) : NULL;
            const char *un = u ? jstr(jget(u, "Name")) : NULL;
            if (t && *t && uid && *uid) {
                snprintf(token, sizeof token, "%.500s", t);
                snprintf(user_id, sizeof user_id, "%.70s", uid);
                if (un) snprintf(user_name, sizeof user_name, "%.200s", un);
            }
            jfree(v);
        }
    }
    resp_free(&r);
    if (!token[0] || !user_id[0])
        return fail("Quick Connect did not return a user token");
    state_lock();
    snprintf(S->token, sizeof S->token, "%s", token);
    snprintf(S->user_id, sizeof S->user_id, "%s", user_id);
    snprintf(S->user_name, sizeof S->user_name, "%s", user_name);
    qc_clear_locked();
    save_token_locked(S->token);
    save_config_locked();
    state_unlock();
    if (jf_validate_token()) {
        jf_log("Quick Connect token did not validate");
        return fail("Quick Connect did not return a user token");
    }
    jf_log("Quick Connect sign-in complete");
    state_lock();
    auth_status_locked(out);
    state_unlock();
    return 0;
}

/* POST /api/auth/logout */
static int auth_logout(struct sb *out) {
    state_lock();
    S->token[0] = 0;
    S->user_id[0] = 0;
    S->user_name[0] = 0;
    S->auth_valid = 0;
    qc_clear_locked();
    save_token_locked(NULL);
    save_config_locked();
    auth_status_locked(out);
    state_unlock();
    jf_log("signed out");
    return 0;
}

/* GET /api/libraries */
static int jf_libraries(struct sb *out) {
    if (require_token()) return -1;
    struct sb path = {0};
    sb_fmt(&path, "Users/%s/Views", S->user_id);
    struct jf_resp r;
    if (jf_get(path.p, &r, 512 * 1024)) { free(path.p); return -1; }
    free(path.p);
    int rc = -1;
    struct jval *v = r.body ? json_parse(r.body, r.body_len) : NULL;
    if (v && r.status == 200) {
        struct jval *items = jget(v, "Items");
        sb_puts(out, "{\"libraries\":[");
        for (size_t i = 0; jnth(items, i); i++) {
            struct jval *it = jnth(items, i);
            if (i) sb_puts(out, ",");
            sb_puts(out, "{\"id\":");
            const char *id = jstr(jget(it, "Id"));
            if (id) sb_json_str(out, id); else sb_puts(out, "null");
            sb_puts(out, ",\"name\":");
            const char *name = jstr(jget(it, "Name"));
            sb_json_str(out, name ? name : "");
            sb_puts(out, ",\"kind\":");
            const char *kind = jstr(jget(it, "CollectionType"));
            sb_json_str(out, kind ? kind : "");
            sb_puts(out, "}");
        }
        sb_puts(out, "]}");
        rc = 0;
    } else {
        fail("Jellyfin returned HTTP %d", r.status);
    }
    jfree(v);
    resp_free(&r);
    return rc;
}

/* Does this item type act as a container the UI can open? */
static int jf_is_container(const char *type) {
    if (!type || !type[0]) return 1;
    return !strcmp(type, "CollectionFolder") || !strcmp(type, "Folder") ||
           !strcmp(type, "Series") || !strcmp(type, "Season") ||
           !strcmp(type, "BoxSet") || !strcmp(type, "MusicAlbum") ||
           !strcmp(type, "Playlist") || !strcmp(type, "MusicArtist");
}

/* Serialize one entry of a Jellyfin Items[] array. */
static void items_append(struct sb *out, struct jval *it) {
    sb_puts(out, "{\"id\":");
    const char *id = jstr(jget(it, "Id"));
    if (id) sb_json_str(out, id); else sb_puts(out, "null");
    sb_puts(out, ",\"name\":");
    const char *name = jstr(jget(it, "Name"));
    sb_json_str(out, name ? name : "");
    const char *type = jstr(jget(it, "Type"));
    sb_puts(out, ",\"type\":");
    sb_json_str(out, type ? type : "");
    sb_puts(out, ",\"year\":");
    struct jval *year = jget(it, "ProductionYear");
    if (year && year->t == J_NUM) sb_fmt(out, "%.0f", year->num);
    else sb_puts(out, "null");
    sb_puts(out, ",\"runtime\":");
    struct jval *rt = jget(it, "RunTimeTicks");
    if (rt && rt->t == J_NUM) sb_fmt(out, "%.0f", rt->num);
    else sb_puts(out, "null");
    sb_puts(out, ",\"overview\":");
    const char *ov = jstr(jget(it, "Overview"));
    if (ov) {
        char trunc[404];
        snprintf(trunc, sizeof trunc, "%.400s", ov);
        sb_json_str(out, trunc);
    } else {
        sb_puts(out, "\"\"");
    }
    const char *mt = jstr(jget(it, "MediaType"));
    const char *lt = jstr(jget(it, "LocationType"));
    int playable = mt && !strcmp(mt, "Video") && (!lt || strcmp(lt, "Virtual"));
    /* Containers are what give the library its shape, so tell the UI explicitly
     * that they can be opened rather than played. */
    int is_folder = jf_is_container(type);
    sb_fmt(out, ",\"playable\":%s", playable ? "true" : "false");
    sb_fmt(out, ",\"isFolder\":%s", is_folder ? "true" : "false");
    sb_fmt(out, ",\"resumeSeconds\":%.0f", jnum(jget(jget(it, "UserData"), "PlaybackPositionTicks"), 0) / 10000000.0);
    sb_puts(out, ",\"childCount\":");
    struct jval *cc = jget(it, "ChildCount");
    if (cc && cc->t == J_NUM) sb_fmt(out, "%.0f", cc->num);
    else sb_puts(out, "null");
    sb_puts(out, "}");
}

/* One upstream Items query.  On success *items points into *body, which the
 * caller frees with items_free(). */
static int items_query(const char *types, const char *filters, int recursive,
                       int limit, int offset, const char *parent,
                       const char *search, struct jval **body,
                       struct jval **items, double *total) {
    struct sb path = {0};
    sb_fmt(&path, "Users/%s/Items?Recursive=%s&Limit=%d&StartIndex=%d"
                  "&SortBy=SortName&SortOrder=Ascending"
                  "&ExcludeLocationTypes=Virtual"
                  "&Fields=Overview,RunTimeTicks,ProductionYear,MediaType,Type,ChildCount",
           S->user_id, recursive ? "true" : "false", limit, offset);
    if (parent && parent[0]) {
        sb_puts(&path, "&ParentId=");
        url_encode(&path, parent);
    }
    if (types && types[0]) {
        sb_puts(&path, "&IncludeItemTypes=");
        url_encode(&path, types);
    }
    if (filters && filters[0]) {
        sb_puts(&path, "&Filters=");
        url_encode(&path, filters);
    }
    if (search && search[0]) {
        sb_puts(&path, "&SearchTerm=");
        url_encode(&path, search);
    }
    struct jf_resp r;
    if (jf_get(path.p, &r, RESP_CAP)) { free(path.p); return -1; }
    free(path.p);
    if (!r.body || r.status != 200) {
        if (!total) fail("Jellyfin returned HTTP %d", r.status);
        resp_free(&r);
        return -1;
    }
    *body = json_parse(r.body, r.body_len);
    resp_free(&r);
    if (!*body) return -1;
    *items = jget(*body, "Items");
    if (total) *total = jnum(jget(*body, "TotalRecordCount"), 0);
    return 0;
}

#define JF_CONTAINERS "CollectionFolder,Folder,Series,Season,BoxSet,MusicAlbum,Playlist,MusicArtist"
#define JF_MEDIA      "Movie,Episode,Video,MusicVideo,Audio"

/* GET /api/items
 *
 * Browsing must preserve the library's own organization.  A library root holds
 * loose movies *alongside* Folder, Series and Season containers; flattening it
 * with Recursive=true both hides those containers and scatters a show's
 * episodes among unrelated standalone movies.
 *
 * So a browse asks for one container's direct children (Recursive=false) and
 * does not constrain the item types, which is what lets folders come back at
 * all.  Jellyfin interleaves folders with files alphabetically, though, which
 * on a 500-item library hides the folders on page 40.  A browse therefore runs
 * containers and media as two ordered passes and serves the containers first,
 * so the organizing structure is what you land on.
 *
 * The un-parented "All titles" view keeps the old recursive + video-only
 * behaviour, because a flat list is the right answer when no library is picked.
 */
/* Serve one page of the concatenated "containers, then media" sequence.
 *
 * A page must be a contiguous slice of that sequence.  Returning containers
 * alone whenever the request starts inside the container run loses items: a
 * 6-card page starting at offset 5 of 5 containers would show 5 cards and the
 * first media item would sit at an offset the pager never asks for.  So each
 * page fills from the container run and then tops up from the media run.
 *
 * mode 0 - browse:   both halves are direct-children queries, split by
 *                    IncludeItemTypes.
 * mode 1 - search:   IncludeItemTypes discards SearchTerm, so the container
 *                    half uses Filters=IsFolder and the media half is pulled
 *                    unfiltered and partitioned here.
 */
static int serve_split(struct sb *out, int mode, const char *parent,
                       const char *search, double folder_total,
                       double media_total, int offset, int limit) {
    int want = limit, emitted = 0;
    long folder_at = offset;
    long media_at = (offset > (int)folder_total) ? offset - (long)folder_total : 0;
    long media_cursor = 0;
    int media_done = 0;
    double total = folder_total + media_total;

    sb_fmt(out, "{\"total\":%.0f,\"items\":[", total);
    while (emitted < want) {
        if (folder_at < (long)folder_total) {
            int batch = want - emitted;
            if (batch > 200) batch = 200;
            struct jval *b = NULL, *it = NULL;
            int bad = mode
                ? items_query(NULL, "IsFolder", 1, batch, (int)folder_at, parent, search, &b, &it, NULL)
                : items_query(JF_CONTAINERS, NULL, 0, batch, (int)folder_at, parent, search, &b, &it, NULL);
            if (bad) break;
            int n = 0;
            for (size_t k = 0; jnth(it, k); k++) {
                if (emitted) sb_puts(out, ",");
                items_append(out, jnth(it, k));
                emitted++; folder_at++; n++;
            }
            jfree(b);
            if (n < batch) folder_at = (long)folder_total;   /* run exhausted */
        } else {
            int need = want - emitted;
            if (mode == 0) {
                struct jval *mb = NULL, *mi = NULL;
                if (items_query(JF_MEDIA, NULL, 0, need, (int)(media_at + media_cursor),
                                parent, search, &mb, &mi, NULL)) break;
                for (size_t k = 0; jnth(mi, k); k++) {
                    if (emitted) sb_puts(out, ",");
                    items_append(out, jnth(mi, k));
                    emitted++;
                }
                jfree(mb);
                if (emitted < want) break;
            } else {
                while (need > 0 && !media_done) {
                    struct jval *cb = NULL, *ci = NULL;
                    double ctot = 0;
                    if (items_query(NULL, NULL, 1, 200, (int)media_cursor, parent,
                                    search, &cb, &ci, &ctot)) { media_done = 1; break; }
                    if (!jnth(ci, 0)) { jfree(cb); media_done = 1; break; }
                    for (size_t k = 0; jnth(ci, k); k++) {
                        struct jval *e = jnth(ci, k);
                        if (jf_is_container(jstr(jget(e, "Type")))) continue;
                        if (media_at > 0) { media_at--; continue; }
                        if (emitted) sb_puts(out, ",");
                        items_append(out, e);
                        emitted++; need--;
                        if (need <= 0) break;
                    }
                    jfree(cb);
                    media_cursor += 200;
                    if (media_cursor >= ctot) media_done = 1;
                }
                if (media_done || emitted < want) break;
            }
        }
    }
    sb_puts(out, "]}");
    return 0;
}

/* GET /api/items
 *
 * Browsing must preserve the library's own organization.  A library root holds
 * loose movies *alongside* Folder, Series and Season containers; flattening it
 * with Recursive=true both hides those containers and scatters a show's
 * episodes among unrelated standalone movies.
 *
 * So a browse asks for one container's direct children (Recursive=false) and
 * does not constrain the item types, which is what lets folders come back at
 * all.  Jellyfin interleaves folders with files alphabetically, though, which
 * on a 500-item library hides the folders on page 40, so both a browse and a
 * search are served containers-first via serve_split().
 *
 * The un-parented "All titles" view keeps the old recursive + video-only
 * behaviour, because a flat list is the right answer when no library is picked.
 */
static int jf_items(const char *query, struct sb *out) {
    if (require_token()) return -1;
    char parent[JF_ID_MAX] = "", search[128] = "";
    char limit_s[16] = "", offset_s[16] = "", video_only[8] = "";
    query_param(query, "parent", parent, sizeof parent);
    query_param(query, "search", search, sizeof search);
    query_param(query, "limit", limit_s, sizeof limit_s);
    query_param(query, "offset", offset_s, sizeof offset_s);
    query_param(query, "videoOnly", video_only, sizeof video_only);
    int limit = atoi(limit_s);
    if (limit < 1) limit = 60;
    if (limit > 200) limit = 200;
    int offset = atoi(offset_s);
    if (offset < 0) offset = 0;

    struct jval *body = NULL, *items = NULL;
    double total = 0;

    if (search[0]) {
        /* Search cannot reuse the browse split.  Verified Jellyfin behaviour:
         * Recursive=false + SearchTerm ignores SearchTerm entirely (514 of
         * 514), and IncludeItemTypes + SearchTerm drops every container.  So a
         * search is recursive, and its container half is Filters=IsFolder.
         * (IsFolder=true is accepted by Jellyfin and silently ignored; only
         * the Filters form actually filters.) */
        double folder_total = 0, match_total = 0, media_total = 0;
        struct jval *fb = NULL, *fi = NULL;
        if (items_query(NULL, "IsFolder", 1, 1, 0, parent[0] ? parent : NULL, search,
                        &fb, &fi, &folder_total)) return -1;
        jfree(fb);
        struct jval *ub = NULL, *ui = NULL;
        if (items_query(NULL, NULL, 1, 1, 0, parent[0] ? parent : NULL, search,
                        &ub, &ui, &match_total)) return -1;
        jfree(ub);
        media_total = match_total - folder_total;
        if (media_total < 0) media_total = 0;
        return serve_split(out, 1, parent[0] ? parent : NULL, search,
                           folder_total, media_total, offset, limit);
    }

    if (!parent[0]) {
        /* Flat "All titles": recursive and, when asked, playable video only. */
        const char *types = (!strcmp(video_only, "1")) ? JF_MEDIA : "";
        if (items_query(types, NULL, 1, limit, offset, NULL, search, &body, &items, &total)) return -1;
        sb_fmt(out, "{\"total\":%.0f,\"items\":[", total);
        for (size_t i = 0; jnth(items, i); i++) {
            if (i) sb_puts(out, ",");
            items_append(out, jnth(items, i));
        }
        sb_puts(out, "]}");
        jfree(body);
        return 0;
    }

    /* Browse: count both halves, then serve one contiguous containers-first page. */
    double folder_total = 0, media_total = 0;
    struct jval *fb = NULL, *fi = NULL;
    if (items_query(JF_CONTAINERS, NULL, 0, 1, 0, parent, search, &fb, &fi, &folder_total)) return -1;
    jfree(fb);
    struct jval *mb = NULL, *mi = NULL;
    if (items_query(JF_MEDIA, NULL, 0, 1, 0, parent, search, &mb, &mi, &media_total)) return -1;
    jfree(mb);
    return serve_split(out, 0, parent, search, folder_total, media_total, offset, limit);
}

/* The proven PlaybackInfo request profile (MPEG-TS, H.264, AC3). */
static void playbackinfo_body(struct sb *b, const char *item_id, double start_seconds, int bitrate) {
    bitrate = jf_quality_safe(bitrate);
    sb_puts(b, "{\"UserId\":\"");
    sb_puts(b, S->user_id);
    sb_puts(b, "\",\"MediaSourceId\":\"");
    sb_puts(b, item_id);
    sb_fmt(b, "\",\"StartTimeTicks\":%lld,\"DeviceProfile\":{\"Name\":\"DIRECTV HR54 MPEG-TS H264 AC3\",",
           (long long)(start_seconds * 10.0e6));
    sb_fmt(b, "\"MaxStreamingBitrate\":%d,", bitrate);
    sb_puts(b,
                "\"DirectPlayProfiles\":[],"
                "\"TranscodingProfiles\":[{\"Container\":\"ts\",\"Type\":\"Video\","
                "\"Protocol\":\"http\",\"VideoCodec\":\"h264\",\"AudioCodec\":\"ac3\","
                "\"MaxAudioChannels\":\"2\"}]},"
                "\"EnableDirectPlay\":false,\"EnableDirectStream\":false,"
                "\"EnableTranscoding\":true,\"AllowVideoStreamCopy\":");
    sb_puts(b, start_seconds > 0 ? "false" : "true");
    sb_puts(b, ",\"AllowAudioStreamCopy\":false,\"MaxAudioChannels\":2,"
               "\"MaxStreamingBitrate\":");
    sb_fmt(b, "%d}", bitrate);
}

/* Never print URL/path/title/query strings: even codec fields are untrusted.
 * h264 in TranscodingUrl is a requested codec, not proof of re-encoding.
 * Actual copy vs encode is verified from Jellyfin's FFmpeg logs in live tests. */
static const char *jf_codec_name(const char *codec) {
    static const char *known[] = {"h264", "hevc", "av1", "vp9", "mpeg2video", "ac3", "aac", "copy"};
    for (size_t i = 0; codec && i < sizeof known / sizeof known[0]; i++)
        if (!strcasecmp(codec, known[i])) return known[i];
    return "unknown";
}
static double jf_diag_number(double n) {
    return isfinite(n) && n >= 0 && n <= 1000000000 ? n : 0;
}
static double jf_url_number(const char *url, const char *key) {
    const char *q = strchr(url, '?'); char raw[32] = "", *end;
    if (!q || !query_param(q + 1, key, raw, sizeof raw)) return 0;
    double n = strtod(raw, &end);
    return *end ? 0 : jf_diag_number(n);
}
static void jf_playback_diagnostic(struct jval *src, const char *url, int bitrate, double start) {
    struct jval *video = NULL, *streams = jget(src, "MediaStreams");
    for (size_t i = 0; jnth(streams, i); i++) {
        struct jval *v = jnth(streams, i);
        const char *type = jstr(jget(v, "Type"));
        if (type && !strcmp(type, "Video")) { video = v; break; }
    }
    const char *q = strchr(url, '?');
    char vc[32] = "", ac[32] = "", subtitle[32] = "";
    if (q) {
        query_param(q + 1, "VideoCodec", vc, sizeof vc);
        query_param(q + 1, "AudioCodec", ac, sizeof ac);
        query_param(q + 1, "SubtitleMethod", subtitle, sizeof subtitle);
    }
    double source_video = jf_diag_number(jnum(jget(video, "BitRate"), 0));
    double target = jf_url_number(url, "VideoBitrate");
    const char *codec = jf_codec_name(jstr(jget(video, "Codec")));
    int encode = start > 0 || strcmp(codec, "h264") || !strcmp(subtitle, "Encode") ||
                 (source_video > 0 && target > 0 && source_video > target);
    jf_log("Jellyfin playback: source %.0fx%.0f %s video=%.0f source-total=%.0f; "
           "requested ceiling=%d; negotiation=%s; output=%s/%s MPEG-TS HTTP; "
           "video-target=%.0f audio-target=%.0f; video-copy=%s audio-copy=disabled",
           jf_diag_number(jnum(jget(video, "Width"), 0)), jf_diag_number(jnum(jget(video, "Height"), 0)),
           codec, source_video, jf_diag_number(jnum(jget(src, "Bitrate"), 0)), bitrate,
           encode ? "encode-required" : "copy-eligible (server decides)",
           jf_codec_name(vc), jf_codec_name(ac), target, jf_url_number(url, "AudioBitrate"),
           start > 0 ? "disabled for offset timestamps" : "allowed");
}

/* Items/<id> sanity checks mirroring server.py's transcode_url prelude. */
static int check_item_video(const char *item_id, char *name, size_t namesz, double *duration) {
    struct sb path = {0};
    sb_fmt(&path, "Items/%s", item_id);
    struct jf_resp r;
    if (jf_get(path.p, &r, 512 * 1024)) { free(path.p); return -1; }
    free(path.p);
    int rc = -1;
    struct jval *v = r.body ? json_parse(r.body, r.body_len) : NULL;
    if (v && r.status == 200) {
        const char *mt = jstr(jget(v, "MediaType"));
        const char *lt = jstr(jget(v, "LocationType"));
        if (!mt || strcmp(mt, "Video")) fail("item is not video");
        else if (lt && !strcmp(lt, "Virtual"))
            fail("this is a Jellyfin virtual episode with no media file");
        else {
            struct jval *sources = jget(v, "MediaSources");
            int probed = 0;
            for (size_t i = 0; jnth(sources, i); i++) {
                struct jval *ms = jget(jnth(sources, i), "MediaStreams");
                if (ms && ms->n) probed = 1;
            }
            if (sources && sources->n && !probed)
                fail("Jellyfin has no probed audio/video streams for this title; "
                     "refresh its media information in Jellyfin");
            else {
                const char *nm = jstr(jget(v, "Name"));
                snprintf(name, namesz, "%.200s", nm ? nm : "");
                double ticks = jnum(jget(v, "RunTimeTicks"), 0);
                *duration = isfinite(ticks) && ticks > 0 && ticks <= 604800.0 * 10000000.0 ? ticks / 10000000.0 : 0;
                rc = 0;
            }
        }
    } else {
        fail("Jellyfin returned HTTP %d", r.status);
    }
    jfree(v);
    resp_free(&r);
    return rc;
}

static int transcode_url(const char *item_id, double start_seconds,
                         char *name, size_t namesz, char *upath, size_t upathsz,
                         char *session, size_t sessz, double *duration) {
    if (session && sessz) session[0] = 0;
    if (check_item_video(item_id, name, namesz, duration)) return -1;
    struct sb body = {0};
    state_lock(); int bitrate = jf_quality_safe(S->jf_quality_bitrate); state_unlock();
    playbackinfo_body(&body, item_id, start_seconds, bitrate);
    struct sb path = {0};
    sb_fmt(&path, "Items/%s/PlaybackInfo", item_id);
    for (int attempt = 0; attempt < 3; attempt++) {
        struct jf_resp r;
        if (jf_post(path.p, body.p, &r, 512 * 1024)) {
            free(path.p);
            free(body.p);
            return -1;
        }
        struct jval *v = r.body ? json_parse(r.body, r.body_len) : NULL;
        int found = 0, no_compatible = 0, bad_status = 0;
        if (v && r.status == 200) {
            struct jval *sources = jget(v, "MediaSources");
            for (size_t i = 0; jnth(sources, i) && !found; i++) {
                struct jval *src = jnth(sources, i);
                const char *tu = jstr(jget(src, "TranscodingUrl"));
                const char *tc = jstr(jget(src, "TranscodingContainer"));
                if (tu && *tu && tc && !strcmp(tc, "ts")) {
                    if (strlen(tu) >= upathsz - 64) fail("transcode URL too long");
                    else {
                        const char *psid = jstr(jget(v, "PlaySessionId"));
                        if (session && sessz && psid && strlen(psid) < sessz)
                            snprintf(session, sessz, "%s", psid);
                        snprintf(upath, upathsz, "%s", tu);
                        found = 1;
                        jf_playback_diagnostic(src, tu, bitrate, start_seconds);
                    }
                }
            }
            const char *ec = jstr(jget(v, "ErrorCode"));
            if (!found && ec && !strcmp(ec, "NoCompatibleStream")) no_compatible = 1;
        } else if (!found) {
            fail("Jellyfin returned HTTP %d", r.status);
            bad_status = 1;
        }
        jfree(v);
        resp_free(&r);
        if (found) {
            if (start_seconds > 0) {
                long long ticks = (long long)(start_seconds * 10.0e6);
                /* Replace duplicate offset/options. Preserve input timestamps:
                 * Jellyfin subtitle filters subtract the seek offset themselves;
                 * resetting input timestamps first drops offset seconds of video. */
                struct sb seek_url = {0};
                const char *query = strchr(upath, '?');
                char copy_timestamps[16] = "";
                if (query) query_param(query + 1, "CopyTimestamps", copy_timestamps, sizeof copy_timestamps);
                jf_log("Jellyfin offset %.0fs: original CopyTimestamps=%s", start_seconds,
                       copy_timestamps[0] ? copy_timestamps : "unspecified");
                if (query) {
                    sb_putn(&seek_url, upath, (size_t)(query - upath));
                    const char *part = query + 1;
                    int count = 0;
                    while (*part) {
                        const char *end = strchr(part, '&');
                        size_t n = end ? (size_t)(end - part) : strlen(part);
                        int override = (n >= 15 && (!strncasecmp(part, "StartTimeTicks=", 15) ||
                                                    !strncasecmp(part, "CopyTimestamps=", 15))) ||
                                       (n >= 21 && (!strncasecmp(part, "AllowVideoStreamCopy=", 21) ||
                                                    !strncasecmp(part, "EnableAutoStreamCopy=", 21)));
                        if (!override) {
                            sb_puts(&seek_url, count++ ? "&" : "?");
                            sb_putn(&seek_url, part, n);
                        }
                        if (!end) break;
                        part = end + 1;
                    }
                    sb_fmt(&seek_url, "%sStartTimeTicks=%lld&CopyTimestamps=true&AllowVideoStreamCopy=false&EnableAutoStreamCopy=false", count ? "&" : "?", ticks);
                } else sb_fmt(&seek_url, "%s?StartTimeTicks=%lld&CopyTimestamps=true&AllowVideoStreamCopy=false&EnableAutoStreamCopy=false", upath, ticks);
                if (!seek_url.p || seek_url.len >= upathsz) {
                    free(seek_url.p); free(path.p); free(body.p);
                    return fail("seek transcode URL too long");
                }
                snprintf(upath, upathsz, "%s", seek_url.p);
                free(seek_url.p);
            }
            free(path.p);
            free(body.p);
            return 0;
        }
        if (no_compatible) {
            free(path.p);
            free(body.p);
            return fail("Jellyfin reports no compatible stream for this title");
        }
        if (bad_status) {
            free(path.p);
            free(body.p);
            return -1;
        }
        if (attempt + 1 < 3) sleep(2);
    }
    free(path.p);
    free(body.p);
    return fail("Jellyfin offered no MPEG-TS transcode (tried 3 times)");
}

