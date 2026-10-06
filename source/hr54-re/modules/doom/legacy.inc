/* Transitional WebKit frame/input adapter for the old distribution daemon. */
static int doom_engine_present(void) {
    char path[512];
    if (doom_path(path, sizeof path, "bin/hr54-doom")) return 0;
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IXUSR);
}

/* The frame pipeline is verifiable without a WAD: dropping this file in
 * makes the launcher start the engine in self-test mode, which paints a
 * known pattern instead of a game.  Used to prove transport, presentation
 * and the kill switch before the WAD is involved. */
static int doom_selftest(void) {
    char path[512];
    struct stat st;
    if (doom_path(path, sizeof path, "selftest")) return 0;
    return stat(path, &st) == 0;
}

/* Launch the DOOM engine.  The engine is a frame producer, not a screen
 * owner: it renders into a file that the TV frontend pulls over HTTP and
 * paints into a fullscreen canvas.  That keeps all drawing inside the
 * already-working WebKit surface, because no process outside the vendor
 * stack can load libdtvwm.so (its load-time init segfaults).
 *
 *   argv[1] WAD          patch to load
 *   argv[2] frame output file to publish atomically
 *   argv[3] input file to poll for a held-key bitmask
 */
static void rc_launch_doom(void) {
    char bin[512], wad[512], pidf[512], frame[512], input[512];
    doom_path(bin, sizeof bin, "bin/hr54-doom");
    doom_path(wad, sizeof wad, "data/doom1.wad");
    doom_path(pidf, sizeof pidf, "doom.pid");
    doom_path(frame, sizeof frame, "frame.raw");
    doom_path(input, sizeof input, "input");
    struct stat st;
    int self = doom_selftest();
    if (stat(wad, &st) && !self) {
        jf_log("GUIDE: DOOM wad missing at %s", wad);
        return;
    }
    if (!doom_engine_present()) {
        jf_log("GUIDE: DOOM engine missing at %s", bin);
        return;
    }
    if (doom_running_pid() > 0) return;
    jf_log("GUIDE: launching DOOM%s", self ? " (selftest)" : "");
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(bin, "hr54-doom", self ? "selftest" : wad, frame, input, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        FILE *f = fopen(pidf, "w");
        if (f) {
            fprintf(f, "%d\n", (int)pid);
            fclose(f);
        }
        jf_log("GUIDE: DOOM pid %d", (int)pid);
    } else jf_log("GUIDE: fork failed");
}

/* ------------------------------------------------------------------ */
/* DOOM frame transport                                               */
/* ------------------------------------------------------------------ */
/* The engine never touches the TV.  It publishes finished frames to
 * doom/frame.raw and the TV frontend paints them, which keeps every
 * drawing operation inside the WebKit surface that already works.
 *
 * Frame file layout, all integers little-endian so the browser can read
 * them with DataView without knowing the box is big-endian:
 *
 *   0    "HRDF"                 magic
 *   4    u32 version            1
 *   8    u32 width
 *   12   u32 height
 *   16   u32 seq                increments once per published frame
 *   20   256 * 4 bytes palette  R,G,B,A per entry
 *   1044 width*height bytes     one palette index per pixel
 *
 * The engine renames a fully written temporary over the real name, so a
 * reader either sees a whole old frame or a whole new one, never a tear.
 *
 * Transport is base64 text rather than binary because the ITV WebKit is
 * old enough that XHR responseType="arraybuffer" is not dependable, and
 * text/plain is universally safe.  atob() on the far side undoes it. */

#define DOOM_FRAME_MAGIC "HRDF"
#define DOOM_FRAME_VER 1u
#define DOOM_FRAME_HDR 1044u

/* Held-key bitmask, mirrored in the TV frontend and read by the engine. */
#define DOOM_IN_LEFT    (1u << 0)
#define DOOM_IN_RIGHT   (1u << 1)
#define DOOM_IN_UP      (1u << 2)
#define DOOM_IN_DOWN    (1u << 3)
#define DOOM_IN_FIRE    (1u << 4)
#define DOOM_IN_USE     (1u << 5)
#define DOOM_IN_STRAFE_L (1u << 6)
#define DOOM_IN_STRAFE_R (1u << 7)

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encodes len bytes, allocating the result.  Returns NULL on overflow. */
static char *base64_encode(const unsigned char *in, size_t len) {
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = b64tab[(v >> 6) & 63];
        out[o++] = b64tab[v & 63];
        i += 3;
    }
    if (i < len) {
        unsigned v = (unsigned)in[i] << 16;
        int two = (i + 1 < len);
        if (two) v |= (unsigned)in[i + 1] << 8;
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = two ? b64tab[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = 0;
    return out;
}

static unsigned rd_le32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}

/* Reads the frame sequence number without decoding the pixels. */
static long doom_frame_seq(void) {
    char path[512];
    unsigned char hdr[20];
    if (doom_path(path, sizeof path, "frame.raw")) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, hdr, sizeof hdr);
    close(fd);
    if (n != (ssize_t)sizeof hdr) return -1;
    if (memcmp(hdr, DOOM_FRAME_MAGIC, 4) || rd_le32(hdr + 4) != DOOM_FRAME_VER)
        return -1;
    return (long)rd_le32(hdr + 16);
}

/* GET /doom/frame[?since=N]
 *
 * Sends the published frame as base64, or a bodiless 304 when the caller
 * already has it.  The 304 is the common case: the frontend polls much
 * faster than the engine renders, and not re-sending an unchanged frame
 * keeps the LAN link and the browser's decoder idle. */
static void serve_doom_frame(int fd, const char *method, const char *query) {
    if (!doom_armed()) { send_json_error(fd, 404, "Not Found", method); return; }
    char path[512];
    if (doom_path(path, sizeof path, "frame.raw")) {
        send_json_error(fd, 404, "Not Found", method);
        return;
    }
    long since = -1;
    if (query) {
        const char *s = strstr(query, "since=");
        if (s) since = strtol(s + 6, NULL, 10);
    }
    long seq = doom_frame_seq();
    if (seq < 0) { send_json_error(fd, 404, "Not Found", method); return; }
    if (since >= 0 && since == seq) {
        state_lock();
        if (S) S->doom_frames_304++;
        state_unlock();
        send_body(fd, 304, http_reason(304), "text/plain; charset=utf-8",
                  "no-store, no-cache, must-revalidate", "", 0, method);
        return;
    }
    size_t len = 0;
    unsigned char *raw = (unsigned char *)read_file(path, &len);
    if (!raw) { send_json_error(fd, 404, "Not Found", method); return; }
    char *b64 = base64_encode(raw, len);
    free(raw);
    if (!b64) { send_json_error(fd, 500, "Internal Server Error", method); return; }
    state_lock();
    if (S) S->doom_frames_served++;
    state_unlock();
    send_body(fd, 200, "OK", "text/plain; charset=utf-8",
              "no-store, no-cache, must-revalidate", b64, strlen(b64), method);
    free(b64);
}

/* Held-key bitmask the engine polls.  Written atomically so the engine
 * never reads a half-updated value. */
#include "input.inc"

static void api_doom_status(int fd) {
    long seq = doom_frame_seq();
    int pid = doom_running_pid();
    struct sb b = {0};
    sb_fmt(&b, "{\"native\":%s,\"armed\":", native_frontend ? "true" : "false");
    sb_puts(&b, doom_armed() ? "true" : "false");
    sb_puts(&b, ",\"running\":");
    sb_puts(&b, (native_frontend ? native_doom_busy() : pid > 0) ? "true" : "false");
    sb_puts(&b, ",\"pid\":");
    char num[32];
    snprintf(num, sizeof num, "%d", pid > 0 ? pid : 0);
    sb_puts(&b, num);
    sb_puts(&b, ",\"seq\":");
    snprintf(num, sizeof num, "%ld", seq < 0 ? -1 : seq);
    sb_puts(&b, num);
    state_lock();
    unsigned long served = S ? S->doom_frames_served : 0;
    unsigned long notmod = S ? S->doom_frames_304 : 0;
    state_unlock();
    sb_puts(&b, ",\"served\":");
    snprintf(num, sizeof num, "%lu", served);
    sb_puts(&b, num);
    sb_puts(&b, ",\"notModified\":");
    snprintf(num, sizeof num, "%lu", notmod);
    sb_puts(&b, num);
    sb_puts(&b, "}");
    send_json_sb(fd, 200, "OK", &b, "GET");
    free(b.p);
}
