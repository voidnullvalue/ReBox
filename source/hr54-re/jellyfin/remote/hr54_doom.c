/* hr54-doom - frame-producing DOOM engine for the DIRECTV HR54.
 *
 * The engine does not own the TV.  There is no usable display on this box:
 * /dev/fb0..3 return ENXIO, there is no /dev/dri, and libdtvwm.so segfaults
 * when any process outside the vendor stack tries to load it.  So the engine
 * renders into memory and publishes finished frames, and the TV frontend
 * paints them into a canvas over the existing WebKit surface.
 *
 * Transport is a palette-indexed frame written atomically to a file:
 *
 *   usage: hr54-doom selftest FRAMEOUT INPUTFILE
 *
 * The frame format is documented in hr54_jf.c next to serve_doom_frame().
 * All header integers are little-endian regardless of the host, because the
 * consumer is a browser.
 *
 * Input is a held-key bitmask in a small file that the engine polls each
 * tick, so held movement works without any per-event IPC.
 */

#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define FW 320
#define FH 200
#define HDR_BYTES 1044
#define PAL_BYTES (HDR_BYTES - 20)

/* Must match the backend's bit assignments. */
#define IN_LEFT     (1u << 0)
#define IN_RIGHT    (1u << 1)
#define IN_UP       (1u << 2)
#define IN_DOWN     (1u << 3)
#define IN_FIRE     (1u << 4)
#define IN_USE      (1u << 5)
#define IN_STRAFE_L (1u << 6)
#define IN_STRAFE_R (1u << 7)

#define FRAME_HZ 15

static volatile sig_atomic_t stop_flag = 0;

static void on_term(int sig) {
    (void)sig;
    stop_flag = 1;
}

static void put_le32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

/* 16 saturated bar colours, then a 240-step grey ramp. */
static void build_palette(unsigned char *pal) {
    static const unsigned char base[16][3] = {
        {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
        {255, 0, 255}, {255, 0, 0}, {0, 0, 255}, {0, 0, 0},
        {255, 128, 0}, {128, 255, 0}, {0, 255, 128}, {128, 0, 255},
        {255, 0, 128}, {0, 128, 255}, {128, 255, 255}, {192, 192, 192},
    };
    int i;
    for (i = 0; i < 16; i++) {
        unsigned char *p = pal + i * 4;
        p[0] = base[i][0];
        p[1] = base[i][1];
        p[2] = base[i][2];
        p[3] = 255;
    }
    for (i = 16; i < 256; i++) {
        unsigned char v = (unsigned char)(((i - 16) * 255) / 239);
        unsigned char *p = pal + i * 4;
        p[0] = p[1] = p[2] = v;
        p[3] = 255;
    }
}

static void fill_rect(unsigned char *fb, int x0, int y0, int w, int h,
                      unsigned char idx) {
    int y;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > FW) w = FW - x0;
    if (y0 + h > FH) h = FH - y0;
    if (w <= 0 || h <= 0) return;
    for (y = 0; y < h; y++)
        memset(fb + (y0 + y) * FW + x0, idx, (size_t)w);
}

/* A pattern that makes transport failures obvious at a glance and proves
 * frames are advancing and the input path is live:
 *   - 16 colour bars across the top
 *   - a grey ramp, so banding or endianness errors are visible
 *   - a bright block whose horizontal position tracks the held-key mask
 *   - a binary strip in the bottom-left showing the low bits of the frame
 *     counter, so frame progress is readable without a status display
 */
static void render_selftest(unsigned char *fb, unsigned frame, unsigned held) {
    int i, x, y;

    for (i = 0; i < 16; i++) {
        int w = FW / 16;
        int x0 = i * w;
        int ww = (i == 15) ? FW - x0 : w;
        fill_rect(fb, x0, 0, ww, 40, (unsigned char)i);
    }

    for (x = 0; x < FW; x++) {
        unsigned char v = (unsigned char)(16 + (x * 239) / (FW - 1));
        for (y = 40; y < 80; y++) fb[y * FW + x] = v;
    }

    fill_rect(fb, 0, 80, FW, FH - 80, 7);

    /* Two markers that move opposite ways with the left/right bits. */
    fill_rect(fb, ((frame * 2) % FW), 85, 20, 30, 4);
    fill_rect(fb, (FW - 20 - ((frame * 2) % FW)), 85, 20, 30, 6);

    /* Held-key readout: one lit cell per bit, so a stuck or missing input
       file is visible without attaching a debugger. */
    for (i = 0; i < 8; i++)
        fill_rect(fb, 4 + i * 12, 120, 10, 10, (held & (1u << i)) ? 3 : 12);

    /* Frame counter, 16 bits, least significant bit leftmost. */
    for (i = 0; i < 16; i++)
        fill_rect(fb, 4 + i * 12, 136, 10, 10, (frame & (1u << i)) ? 15 : 8);

    /* A slow full-width sweep makes dropped or duplicated frames obvious. */
    fill_rect(fb, (int)(frame * 3 % FW), 150, 3, 50, 1);
}

static unsigned read_held(const char *path) {
    FILE *f = fopen(path, "r");
    unsigned mask = 0;
    if (!f) return 0;
    if (fscanf(f, "%u", &mask) != 1) mask = 0;
    fclose(f);
    return mask & 0xffu;
}

/* Publish atomically: write a temporary beside the target and rename over
 * it, so a concurrent reader sees either the old whole frame or the new
 * whole frame and never a torn one. */
static int publish(const char *path, const unsigned char *frame) {
    size_t n = strlen(path);
    char *tmp = malloc(n + 8);
    if (!tmp) return -1;
    memcpy(tmp, path, n);
    memcpy(tmp + n, ".tmp", 5);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(tmp); return -1; }
    size_t wrote = fwrite(frame, 1, HDR_BYTES + FW * FH, f);
    int ok = (wrote == HDR_BYTES + FW * FH);
    if (fclose(f)) ok = 0;
    if (ok && rename(tmp, path)) ok = 0;
    if (!ok) unlink(tmp);
    free(tmp);
    return ok ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : NULL;
    const char *frame_path = argc > 2 ? argv[2] : NULL;
    const char *input_path = argc > 3 ? argv[3] : NULL;

    if (!mode || !frame_path) {
        fprintf(stderr, "usage: hr54-doom selftest FRAMEOUT [INPUTFILE]\n");
        return 2;
    }
    if (strcmp(mode, "selftest") != 0) {
        /* The WAD renderer is not written yet.  Refuse loudly rather than
           showing a plausible-looking picture that is not the game. */
        fprintf(stderr, "hr54-doom: wad mode not implemented yet\n");
        return 3;
    }

    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);

    unsigned char *frame = malloc(HDR_BYTES + FW * FH);
    if (!frame) return 1;
    memcpy(frame, "HRDF", 4);
    put_le32(frame + 4, 1);
    put_le32(frame + 8, FW);
    put_le32(frame + 12, FH);
    build_palette(frame + 20);

    unsigned char *fb = frame + HDR_BYTES;
    const long period_ns = 1000000000L / FRAME_HZ;
    unsigned n = 0;
    while (!stop_flag) {
        struct timespec ts;
        unsigned held = input_path ? read_held(input_path) : 0;
        render_selftest(fb, n, held);
        put_le32(frame + 16, n);
        if (publish(frame_path, frame) != 0) {
            fprintf(stderr, "hr54-doom: cannot publish %s\n", frame_path);
            free(frame);
            return 1;
        }
        n++;
        ts.tv_sec = 0;
        ts.tv_nsec = period_ns;
        nanosleep(&ts, NULL);
    }
    free(frame);
    return 0;
}
