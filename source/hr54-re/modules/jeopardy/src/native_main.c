#define _POSIX_C_SOURCE 200809L
/*
 * Native Jeopardy TV renderer (uClibc / vendor EGL).
 *
 * A dumb consumer of the authoritative service: it reads ONLY the redacted
 * public-display.json plus the private metadata.json (URL/PIN/hostPIN) and sends
 * trusted commands on the private command.sock. It never reads the LAN, never
 * sees the host snapshot and never runs rules. Renders at a bounded 20 fps and
 * keeps animating while a timer runs or results/confetti are on screen.
 */
#include "jpq_view.h"
#include "jpq_render.h"
#include "jpq_remote.h"
#include "jpq_input.h"
#include "native_egl.h"
#include "receiver_runtime.h"
#include "receiver-include/libc.h"
#include <stdint.h>

#define AF_UNIX_LOCAL 1
struct jpq_sun { unsigned short sun_family; char sun_path[108]; };

static volatile sig_atomic_t stopping;
static void on_stop(int sig) { (void)sig; stopping = 1; }

static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

static int slurp(const char *path, char *buf, size_t cap, size_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (size_t)n >= cap) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) return -1;
    buf[got] = 0;
    if (out) *out = got;
    return 0;
}

static uint32_t hash32(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

/* ------------------------------------------------------------------ */
/* Bounded, non-blocking private command channel                        */
/* ------------------------------------------------------------------ */
/* The socket is only ever written in bounded chunks and read back for the
 * service's ok/error reply; a slow or dead peer degrades to a status banner
 * instead of blocking the renderer. */

static int cmd_fd = -1;
static const char *g_sock;
static JpqRemote *g_remote;
static JpqView *g_view;
static uint64_t cmd_retry_at;
static char cmd_out[2048];
static size_t cmd_out_len;
static char cmd_in[512];
static size_t cmd_in_len;
static char g_status[80];
static uint64_t g_status_until;
static int g_remote_dirty;

static void status_set(const char *msg) {
    size_t i = 0;
    if (msg) for (; msg[i] && i + 1 < sizeof g_status; i++) g_status[i] = msg[i];
    g_status[i] = 0;
    g_status_until = now_ms() + 2500;
}

static int cmd_connect(void) {
    if (cmd_fd >= 0 || !g_sock) return cmd_fd >= 0 ? 0 : -1;
    uint64_t now = now_ms();
    if (cmd_retry_at && now < cmd_retry_at) return -1;   /* retry at most ~1/s */
    cmd_retry_at = now + 1000;
    int fd = socket(AF_UNIX_LOCAL, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct jpq_sun sa;
    sa.sun_family = AF_UNIX_LOCAL;
    size_t i = 0;
    for (; g_sock[i] && i < sizeof sa.sun_path - 1; i++) sa.sun_path[i] = g_sock[i];
    sa.sun_path[i] = 0;
    if (connect(fd, (const struct sockaddr *)&sa, (unsigned)(sizeof sa.sun_family + i + 1))) {
        close(fd);
        if (!g_status[0]) status_set("TV CONTROL OFFLINE");
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    cmd_fd = fd;
    return 0;
}

static void cmd_queue(const char *line) {
    size_t n = 0;
    while (line[n]) n++;
    if (n + 1 > sizeof cmd_out) { status_set("command too long"); return; }
    if (cmd_out_len + n + 1 > sizeof cmd_out) {   /* bounded backlog */
        cmd_out_len = 0;
        status_set("command backlog full");
    }
    for (size_t i = 0; i < n; i++) cmd_out[cmd_out_len++] = line[i];
    cmd_out[cmd_out_len++] = '\n';
}

static void cmd_flush(void) {
    if (cmd_fd < 0) return;
    while (cmd_out_len) {
        ssize_t w = send(cmd_fd, cmd_out, cmd_out_len, MSG_NOSIGNAL);
        if (w > 0) {
            size_t k = (size_t)w;
            for (size_t i = k; i < cmd_out_len; i++) cmd_out[i - k] = cmd_out[i];
            cmd_out_len -= k;
            continue;
        }
        if (w < 0 && (errno == EAGAIN)) return;
        close(cmd_fd); cmd_fd = -1; cmd_out_len = 0;
        status_set("TV CONTROL OFFLINE");
        return;
    }
}

/* Report the service's reply for one command line: an error string becomes a
 * visible banner, a success clears any stale error. */
static void cmd_handle_reply(const char *line, size_t len) {
    int ok = 1;
    for (size_t i = 0; i + 10 <= len; i++) {
        if (line[i] == '"' && line[i + 1] == 'o' && line[i + 2] == 'k' && line[i + 3] == '"' &&
            line[i + 4] == ':' && line[i + 5] == 'f' && line[i + 6] == 'a' && line[i + 7] == 'l' &&
            line[i + 8] == 's' && line[i + 9] == 'e') { ok = 0; break; }
    }
    if (ok) { if (g_status[0]) { g_status[0] = 0; } return; }
    char msg[60];
    size_t m = 0;
    const char *key = "\"error\":\"";
    for (size_t i = 0; i + 9 < len; i++) {
        int match = 1;
        for (size_t k = 0; k < 9; k++) if (line[i + k] != key[k]) { match = 0; break; }
        if (!match) continue;
        size_t j = i + 9;
        while (j < len && line[j] != '"' && m + 1 < sizeof msg) msg[m++] = line[j++];
        break;
    }
    msg[m] = 0;
    status_set(m ? msg : "command rejected");
}

static void cmd_read_replies(void) {
    if (cmd_fd < 0) return;
    for (;;) {
        ssize_t r = recv(cmd_fd, cmd_in + cmd_in_len, sizeof cmd_in - cmd_in_len, 0);
        if (r > 0) {
            cmd_in_len += (size_t)r;
            size_t start = 0;
            for (size_t i = 0; i < cmd_in_len; i++) {
                if (cmd_in[i] != '\n') continue;
                size_t end = i;
                if (end > start && cmd_in[end - 1] == '\r') end--;
                if (end > start) cmd_handle_reply(cmd_in + start, end - start);
                start = i + 1;
            }
            if (start) {
                for (size_t i = start; i < cmd_in_len; i++) cmd_in[i - start] = cmd_in[i];
                cmd_in_len -= start;
            }
            if (cmd_in_len == sizeof cmd_in) cmd_in_len = 0;   /* drop oversize line */
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN)) return;
        close(cmd_fd); cmd_fd = -1; cmd_in_len = 0;
        status_set("TV CONTROL OFFLINE");
        return;
    }
}

static void dispatch(JvKey key) {
    if (!g_remote || !g_view) return;
    char line[512];
    int rc = jpq_remote_key(g_remote, key, g_view, line, sizeof line);
    if (rc == 1 && g_sock) {
        cmd_connect();
        cmd_queue(line);
        g_remote_dirty = 1;
    } else if (rc == 0) {
        g_remote_dirty = 1;   /* local cursor/menu change must be drawn */
    }
}

int main(int argc, char **argv) {
    int depth = 0, ready_fd = -1, result = 1;
    const char *data = getenv("REBOX_MODULE_DATA");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--depth") && i + 1 < argc) depth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ready-fd") && i + 1 < argc) ready_fd = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else return 2;
    }
    if (!data || !*data) return 2;
    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);

    char meta_path[600], disp_path[600];
    snprintf(meta_path, sizeof meta_path, "%s/native/metadata.json", data);
    snprintf(disp_path, sizeof disp_path, "%s/native/public-display.json", data);

    JpqMetadata meta;
    memset(&meta, 0, sizeof meta);
    char buf[65536];
    size_t n = 0;
    uint32_t meta_hash = 0;
    if (!slurp(meta_path, buf, sizeof buf, &n)) {
        jpq_metadata_parse(&meta, buf, n);
        meta_hash = hash32(buf, n);
    }
    const char *sock = meta.command_socket[0] ? meta.command_socket : 0;

    Hr54EglSurface surface = {0};
    UiFramebuffer frame = {0};

    /* No input broker means no controller: fail readiness rather than come up
     * as an unresponsive surface the room cannot drive. */
    if (jpq_input_open()) {
        fprintf(stderr, "jeopardy-native: input broker unavailable\n");
        goto done;
    }

    if (hr54_egl_open(&surface, &hr54_receiver_egl_api, 720, 480, depth)) goto done;
    surface.aspect_width = 3;
    surface.aspect_height = 2;
    surface.replace_frame = 1;
    if (ui_fb_init(&frame, surface.width, surface.height)) goto done;

    JpqView view;
    memset(&view, 0, sizeof view);
    JpqRemote remote;
    jpq_remote_init(&remote);
    g_sock = sock;
    g_remote = &remote;
    g_view = &view;
    int32_t prev[JPQ_VIEW_PLAYERS];
    for (int i = 0; i < JPQ_VIEW_PLAYERS; i++) prev[i] = 0;

    uint64_t anim = now_ms();
    uint32_t anim_key = 0;
    uint32_t last_hash = 0;
    int have = 0;
    /* Readiness waits for the first valid service view that is not a stale
     * quit, so the SDK never marks a doomed surface running. The relaunch reset
     * normally supplies a fresh lobby/live game well inside the bound. */
    uint64_t view_deadline = now_ms() + 6000;
    for (;;) {
        size_t dn = 0;
        if (!slurp(disp_path, buf, sizeof buf, &dn) && !jpq_view_parse(&view, buf, dn)) {
            last_hash = hash32(buf, dn);
            have = 1;
            for (int i = 0; i < JPQ_VIEW_PLAYERS; i++) prev[i] = view.players[i].score;
            if (!view.quit) break;
        }
        if (now_ms() >= view_deadline) break;
        struct timespec wait = {0, 5000000};
        nanosleep(&wait, 0);
    }
    if (have) {
        anim_key = view.epoch * 2654435761u ^ (view.window << 8) ^ ((uint32_t)view.phase << 4) ^ view.round;
        if (view.quit) stopping = 1;   /* a real committed quit, not a stale read */
    }
    jpq_render(&frame, have ? &view : 0, &meta, anim, anim, prev);
    jpq_render_overlay(&frame, &remote, &meta, &view);
    jpq_render_status(&frame, g_status);
    if (hr54_egl_submit(&surface, &frame, (UiRect){0, 0, frame.width, frame.height})) goto done;
    if (ready_fd >= 0) {
        unsigned char ok = 1;
        if (write(ready_fd, &ok, 1) != 1) goto done;
        close(ready_fd);
        ready_fd = -1;
    }

    uint64_t last_meta = now_ms();
    while (!stopping) {
        /* input -> trusted command */
        jpq_input_pump(dispatch);
        uint64_t now = now_ms();

        int dirty = 0;
        /* Re-read metadata occasionally so a restarted service's new PIN/URL shows. */
        if (now - last_meta > 5000) {
            last_meta = now;
            size_t mn = 0;
            if (!slurp(meta_path, buf, sizeof buf, &mn)) {
                uint32_t h = hash32(buf, mn);
                if (h != meta_hash) {
                    JpqMetadata fresh;
                    if (!jpq_metadata_parse(&fresh, buf, mn)) { meta = fresh; meta_hash = h; dirty = 1; }
                }
            }
        }
        size_t dn = 0;
        if (!slurp(disp_path, buf, sizeof buf, &dn)) {
            uint32_t h = hash32(buf, dn);
            if (h != last_hash) {
                JpqView fresh;
                if (!jpq_view_parse(&fresh, buf, dn)) {
                    for (int i = 0; i < JPQ_VIEW_PLAYERS; i++) prev[i] = view.players[i].score;
                    view = fresh;
                    have = 1;
                    last_hash = h;
                    dirty = 1;
                    /* Animation key ignores revision/now (which change every
                     * timer tick) so reveals/tweens are not restarted by an
                     * unrelated broadcast; only a new clue/phase resets them. */
                    uint32_t key = view.epoch * 2654435761u ^ (view.window << 8) ^
                                   ((uint32_t)view.phase << 4) ^ view.round;
                    if (key != anim_key) { anim_key = key; anim = now; }
                    if (view.quit) stopping = 1;
                }
            }
        }

        /* The scoreboard tween, reveal animations, the live countdown and the
         * winner confetti all need continuous frames; a static board does not. */
        int anim_active = (now - anim) < 1600;
        int live = have && !view.paused &&
                   (view.timer_kind != JV_TIMER_NONE || view.phase == JV_RESULTS);
        if (dirty || g_remote_dirty || anim_active || live || (g_status[0] && now < g_status_until)) {
            g_remote_dirty = 0;
            jpq_render(&frame, have ? &view : 0, &meta, now, anim, prev);
            jpq_render_overlay(&frame, &remote, &meta, &view);
            if (now < g_status_until) jpq_render_status(&frame, g_status);
            if (hr54_egl_submit(&surface, &frame, (UiRect){0, 0, frame.width, frame.height})) break;
        }

        /* Flush queued commands and drain replies without blocking. */
        cmd_flush();
        cmd_read_replies();

        struct timespec delay = {0, 50000000}; /* 20 fps */
        nanosleep(&delay, 0);
    }
    result = 0;
done:
    if (ready_fd >= 0) close(ready_fd);
    if (cmd_fd >= 0) close(cmd_fd);
    jpq_input_close();
    if (surface.initialized && hr54_egl_close(&surface)) result = 1;
    ui_fb_free(&frame);
    return result;
}
