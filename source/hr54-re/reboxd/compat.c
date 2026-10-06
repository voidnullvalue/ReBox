#include "compat.h"
#include "module_rpc.h"
#include "playback.h"

/* Service names here are legacy wire contracts, never runtime discovery or
 * generic dispatch. Operations and response translation remain module-owned. */
static const struct { const char *method, *old, *module, *operation, *rpc_method; } routes[] = {
    {"GET", "/api/auth/status", "jellyfin", "/actions/auth.status", "POST"},
    {"POST", "/api/auth/start", "jellyfin", "/actions/auth.start", "POST"},
    {"GET", "/api/auth/poll", "jellyfin", "/actions/auth.poll", "POST"},
    {"POST", "/api/auth/logout", "jellyfin", "/actions/auth.logout", "POST"},
    {"GET", "/api/libraries", "jellyfin", "/legacy/libraries", "GET"},
    {"GET", "/api/items", "jellyfin", "/legacy/items", "GET"},
    {"GET", "/api/jellyfin/resume", "jellyfin", "/legacy/resume", "GET"},
    {"GET", "/api/settings", "jellyfin", "/legacy/settings", "GET"},
    {"POST", "/api/settings", "jellyfin", "/legacy/settings", "POST"},
    {"GET", "/api/jellyfin/status", "jellyfin", "/legacy/settings", "GET"},
    {"POST", "/api/play", "jellyfin", "/play", "POST"},
};
static int available(const ReboxModule *m) {
    return m && m->installed && m->enabled && m->healthy && m->compatible;
}
int rb_compat(int fd, const RbRequest *q, ReboxRegistry *registry) {
    char path[sizeof q->path]; strcpy(path, q->path);
    char *query = strchr(path, '?'); if (query) *query++ = 0;
    if (!strcmp(q->method, "GET") && !strcmp(path, "/api/capabilities")) {
        const char *ids[] = {"jellyfin", "iptv", "youtube", "frigate", "doom"};
        struct sb out = {0}; sb_puts(&out, "{\"ok\":true");
        for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++)
            sb_fmt(&out, ",\"%s\":%s", ids[i], available(rb_registry_find(registry, ids[i])) ? "true" : "false");
        int settings = available(rb_registry_find(registry, "jellyfin"));
        sb_fmt(&out, ",\"playback\":true,\"settings\":%s,\"settingsWritable\":%s}", settings ? "true" : "false", settings ? "true" : "false");
        rb_http_json(fd, 200, out.p); free(out.p); return 1;
    }
    if (!strcmp(q->method, "GET") && !strcmp(path, "/api/status")) {
        struct sb out = {0}; rb_playback_json(&out); rb_http_json(fd, 200, out.p); free(out.p); return 1;
    }
    if (!strcmp(q->method, "POST") && (!strcmp(path, "/api/transport") || !strcmp(path, "/api/seek"))) {
        struct jval *v = json_parse(q->body, q->length);
        const char *operation = !strcmp(path, "/api/seek") ? "seek" : jstr(jget(v, "action"));
        struct sb out = {0}; int code = operation ? rb_transport(registry, operation, q->body, &out) : 400;
        if (code == 200) rb_http_json(fd, code, out.p); else rb_http_error(fd, code, "transport unavailable");
        free(out.p); jfree(v); return 1;
    }
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        if (strcmp(path, routes[i].old) || strcmp(q->method, routes[i].method)) continue;
        ReboxModule *m = rb_registry_find(registry, routes[i].module);
        if (!m || !m->installed) { rb_http_error(fd, 404, "module not installed"); return 1; }
        if (!available(m)) { rb_http_error(fd, 409, "module unavailable"); return 1; }
        if (!strcmp(routes[i].operation, "/play")) {
            struct sb out = {0}; int code = rb_play(registry, m, q->body, &out);
            if (code == 200) rb_http_json(fd, code, out.p); else rb_http_error(fd, code, "playback preparation failed");
            free(out.p); return 1;
        }
        struct sb operation = {0}; sb_puts(&operation, routes[i].operation);
        if (query) { sb_puts(&operation, "?"); sb_puts(&operation, query); }
        RbReply reply;
        if (rb_rpc(m, routes[i].rpc_method, operation.p, *q->body ? q->body : "{}", &reply, 120)) rb_http_error(fd, 502, "module unavailable");
        else { rb_http_reply(fd, reply.status, "application/json", reply.body.p, reply.body.len); rb_reply_free(&reply); }
        free(operation.p); return 1;
    }
    return 0;
}
