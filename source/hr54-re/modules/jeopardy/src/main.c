#define _GNU_SOURCE
/*
 * Jeopardy service entry point (authoritative LAN service + native lifecycle).
 *
 * The network/event loop owns the game state and runs on a single thread, so
 * every action is serialized. When the shared ReBox SDK is enabled it runs on
 * the main thread (its signal handlers own shutdown) while the LAN poll loop
 * runs on a worker; the SDK's /native routes only fork/kill the renderer under
 * the operation mutex and never read or write rules state, so there is no race
 * with the poll thread. The private native command/display channel is served on
 * a local Unix socket.
 */
#include "jeopardy_service.h"
#include "jeopardy_net.h"
#include "jpq_lifecycle.h"
#include "../../shared/sdk.h"
#include "../../../tools/native-ui/presentation_policy.h"
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>

static JpqNet *g_net;
static JpqService *g_svc;
static const char *data_root;
static char engine_path[1024];
static char pid_path[1200], lock_path[1200];
static pthread_mutex_t operation_mutex = PTHREAD_MUTEX_INITIALIZER;

static void on_signal(int sig) {
    (void)sig;
    if (g_net) jpq_net_stop(g_net);
}

static void net_send(void *ud, JpqConnId conn, const char *data, size_t len) {
    JpqNet *n = ud;
    /* Line connections (private native channel) get newline replies; websocket
     * viewers get frames. Each send rejects the other's connection kind. */
    if (jpq_net_line_send(n, conn, data, len) == 0) return;
    jpq_net_ws_send(n, conn, data, len);
}

static void net_close(void *ud, JpqConnId conn, int code) {
    jpq_net_conn_close((JpqNet *)ud, conn, code);
}

/* ---- native surface lifecycle (never touches rules state) ------------ */
static int native_pid(void) { return jpq_lifecycle_pid(engine_path, pid_path); }
static int native_busy(void) { return jpq_lifecycle_busy(lock_path); }

/* Single source of truth for "the TV surface is up". Written only on the
 * lifecycle thread and read (atomically) by the poll thread, so the game gate
 * and the unexpected-exit monitor never race on rules state. */
static volatile int g_native_active;
static void set_native_active(int active) {
    __atomic_store_n(&g_native_active, active ? 1 : 0, __ATOMIC_SEQ_CST);
    if (g_svc) jpq_service_set_native_active(g_svc, active);
}

static int native_start(struct sb *out) {
    int pid = 0;
    char err[160];
    /* Reset any quit / armed confirmation left by a previous session on the
     * poll thread and wait for the fresh public display before the renderer
     * reads it, so a relaunch never starts on a stale quit=true view. */
    if (g_svc) {
        jpq_service_request_relaunch(g_svc);
        for (int i = 0; i < 400; i++) {            /* bounded ~2s */
            if (jpq_service_relaunch_done(g_svc)) break;
            struct timespec d = {0, 5000000};
            nanosleep(&d, 0);
        }
    }
    if (native_busy()) { set_native_active(1); sb_puts(out, "{\"running\":true}"); return 0; }
    if (jpq_lifecycle_start(engine_path, pid_path, lock_path, HR54_NATIVE_FOREGROUND_DEPTH_ARG,
                            7000, &pid, err, sizeof err))
        return fail("%s", err);
    set_native_active(1);
    sb_fmt(out, "{\"running\":true,\"pid\":%d}", pid);
    return 0;
}

static void native_stop(void) {
    int was = 0;
    /* Drop the active flag first so the poll-thread monitor treats this as an
     * expected stop, not a crash. */
    set_native_active(0);
    jpq_lifecycle_stop(engine_path, pid_path, lock_path, &was);
    /* Returning core ownership: pause an in-progress game so its timers stop
     * until a controller resumes it. Lobby state is left untouched. */
    if (was && g_svc) jpq_service_request_pause(g_svc);
}

/* Poll-thread tick: detect an unexpected native exit, then always advance the
 * service (timers/broadcast). The authoritative LAN loop is never stopped here:
 * a confirmed quit only tears down the native child (which exits itself once it
 * sees quit in the public display), and the service stays alive for the module
 * lifespan so the next /native/start can relaunch against a live LAN. */
static void net_tick(void *ud, uint64_t now) {
    JpqService *s = ud;
    if (__atomic_load_n(&g_native_active, __ATOMIC_ACQUIRE) && !native_busy()) {
        set_native_active(0);
        /* An unexpected exit interrupts the game; a confirmed quit is already
         * going away, so do not pause it. */
        if (s && !jpq_service_quit(s)) jpq_service_request_pause(s);
    }
    jpq_service_tick(s, now);
}

static void sdk_status(struct sb *out) {
    sb_fmt(out, "{\"ok\":true,\"moduleApi\":1,\"available\":%s,\"running\":%s,\"pid\":%d}",
           access(engine_path, X_OK) ? "false" : "true", native_busy() ? "true" : "false", native_pid());
}

static void sdk_handler(int fd, const RbRequest *r) {
    if (!strcmp(r->path, "/status") || !strcmp(r->path, "/native/status")) {
        struct sb out = {0};
        sdk_status(&out);
        rb_http_json(fd, 200, out.p ? out.p : "{}");
        free(out.p);
        return;
    }
    pthread_mutex_lock(&operation_mutex);
    struct sb out = {0};
    int code = 200, rc = 0;
    if (!strcmp(r->path, "/native/start") && !strcmp(r->method, "POST")) rc = native_start(&out);
    else if (!strcmp(r->path, "/native/stop") && !strcmp(r->method, "POST")) {
        native_stop();
        if (native_busy()) code = 409;
        else sb_puts(&out, "{\"running\":false}");
    } else code = 404;
    if (rc) rb_http_error(fd, 502, g_err);
    else if (code != 200) rb_http_error(fd, code, "native lifecycle unavailable");
    else rb_http_json(fd, 200, out.p ? out.p : "{}");
    free(out.p);
    pthread_mutex_unlock(&operation_mutex);
}

static void *net_thread_fn(void *arg) {
    (void)arg;
    jpq_net_run(g_net);
    return NULL;
}

/* Pick a non-loopback, non-link-local IPv4 address to advertise on the LAN,
 * preferring RFC1918 private ranges. Uses getifaddrs only; no external
 * connection or DNS. Returns 0 and fills out on success. */
static int detect_lan_ipv4(char *out, size_t cap) {
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs)) return -1;
    int rc = -1, best = 0;
    for (struct ifaddrs *ifa = ifs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if ((ifa->ifa_flags & (IFF_LOOPBACK | IFF_UP)) != IFF_UP) continue;
        const struct sockaddr_in *sin = (const struct sockaddr_in *)ifa->ifa_addr;
        uint32_t a = ntohl(sin->sin_addr.s_addr);
        if (a == 0 || (a >> 24) == 127 || (a & 0xFFFF0000u) == 0xA9FE0000u) continue;
        int score = 1;
        if ((a & 0xFF000000u) == 0x0A000000u) score = 3;      /* 10/8 */
        else if ((a & 0xFFF00000u) == 0xAC100000u) score = 3; /* 172.16/12 */
        else if ((a & 0xFFFF0000u) == 0xC0A80000u) score = 3; /* 192.168/16 */
        if (score > best && inet_ntop(AF_INET, &sin->sin_addr, out, cap)) { best = score; rc = 0; }
    }
    freeifaddrs(ifs);
    return rc;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    data_root = getenv("JPQ_DATA");
    if (!data_root || !*data_root) data_root = getenv("REBOX_MODULE_DATA");
    if (!data_root || !*data_root) data_root = ".";

    /* Copy package-owned defaults (demo questions) into private data without
     * replacing user files. */
    if (rb_module_seed("default-data", "")) {
        fprintf(stderr, "jeopardy: cannot seed default questions\n");
        return 1;
    }
    if (getenv("REBOX_MODULE_SEED_ONLY")) return 0;

    char dbpath[1200];
    const char *db = getenv("JPQ_DB");
    if (!db || !*db) {
        if (snprintf(dbpath, sizeof dbpath, "%s/questions.jdb", data_root) >= (int)sizeof dbpath) return 2;
        db = dbpath;
    }
    int port = 8135;
    const char *penv = getenv("JPQ_PORT");
    if (penv && *penv) port = atoi(penv);
    const char *bind = getenv("JPQ_BIND");
    if (!bind || !*bind) bind = "0.0.0.0";
    const char *url = getenv("JPQ_PUBLIC_URL");

    /* Fixed native executable next to this module, plus retained lock and pid
     * files in the private data directory. */
    if (!realpath(argv[0], engine_path)) return 2;
    char *slash = strrchr(engine_path, '/');
    if (!slash) return 2;
    *slash = 0;
    size_t len = strlen(engine_path);
    if (snprintf(engine_path + len, sizeof(engine_path) - len, "/hr54-jeopardy-native") >= (int)(sizeof(engine_path) - len)) return 2;
    if (snprintf(pid_path, sizeof pid_path, "%s/native/jeopardy.pid", data_root) >= (int)sizeof pid_path ||
        snprintf(lock_path, sizeof lock_path, "%s/native/native.lock", data_root) >= (int)sizeof lock_path) return 2;

    JpqService *svc = jpq_service_create();
    if (!svc) return 2;
    if (jpq_service_start(svc, data_root, db)) {
        fprintf(stderr, "jeopardy: cannot start service data=%s db=%s\n", data_root, db);
        return 1;
    }
    g_svc = svc;

    JpqNetOps ops;
    memset(&ops, 0, sizeof ops);
    ops.http = jpq_service_http;
    ops.ws_path = jpq_service_ws_path;
    ops.ws_open = jpq_service_ws_open;
    ops.ws_text = jpq_service_ws_text;
    ops.ws_close = jpq_service_ws_close;
    ops.line = jpq_service_line;
    ops.tick = net_tick;

    g_net = jpq_net_create(&ops, svc);
    if (!g_net) return 2;
    int actual = 0;
    if (jpq_net_listen_tcp(g_net, bind, port, &actual)) {
        fprintf(stderr, "jeopardy: cannot bind %s:%d\n", bind, port);
        return 1;
    }
    jpq_net_listen_unix_lines(g_net, jpq_service_command_socket(svc));
    if (access(jpq_service_command_socket(svc), F_OK)) fprintf(stderr, "jeopardy: native command socket unavailable\n");
    jpq_service_set_net(svc, net_send, net_close, g_net);

    /* Honor an explicit public URL, otherwise autodetect a reachable LAN IPv4. */
    char urlbuf[256];
    if (!url || !*url) {
        char ip[64];
        if (detect_lan_ipv4(ip, sizeof ip)) snprintf(ip, sizeof ip, "127.0.0.1");
        snprintf(urlbuf, sizeof urlbuf, "http://%s:%d/", ip, actual);
        url = urlbuf;
    }
    jpq_service_set_port(svc, actual, url);

    signal(SIGPIPE, SIG_IGN);

    struct sb ready = {0};
    sb_fmt(&ready, "JPQ_READY port=%d url=%s data=%s", actual, url ? url : "", data_root);
    if (ready.p) { printf("%s\n", ready.p); fflush(stdout); }
    free(ready.p);

    int has_sdk = getenv("REBOX_MODULE_SOCKET") && getenv("REBOX_MODULE_API");
    /* Installed runs gate LAN gameplay on the native surface; host-only runs
     * (no SDK environment) stay ungated and behave exactly as before. */
    jpq_service_set_native_gate(svc, has_sdk ? 1 : 0);
    set_native_active(native_busy());
    if (has_sdk) {
        pthread_t nt;
        if (pthread_create(&nt, NULL, net_thread_fn, NULL) == 0) {
            /* The shared SDK runs on the main thread; its signal handlers own
             * SIGTERM/SIGINT. When it returns (signal or parent exit) stop the
             * worker deterministically and drive shutdown from here. */
            rb_module_serve(sdk_handler);
            jpq_net_stop(g_net);
            pthread_join(nt, NULL);
            goto shutdown;
        }
        fprintf(stderr, "jeopardy: cannot start network thread; running on main\n");
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    jpq_net_run(g_net);

shutdown:
    jpq_net_destroy(g_net);
    jpq_service_shutdown(svc);
    int quit = jpq_service_quit(svc);
    jpq_service_destroy(svc);
    return quit ? 0 : 0;
}
