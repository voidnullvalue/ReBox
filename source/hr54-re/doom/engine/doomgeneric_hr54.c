/* doomgeneric_hr54.c - the platform layer for the DIRECTV HR54-700.
 *
 * HR54_NATIVE_VIDEO presents completed indexed frames through doom_video,
 * backed by the recovered vendor EGL/drawlist retained compositor. Native
 * keydispatcher events feed the existing Doom event queue. This build needs
 * neither a browser nor the old frame-file transport.
 *
 * The legacy build below preserves the earlier HRDF file/input interface for
 * compatibility. Direct framebuffer/blit experiments did not establish HDMI
 * presentation; the native EGL build is the receiver presentation proof.
 *
 *   usage: hr54-doom <wad> [doomgeneric options]
 *
 * Files, all under $HR54_DOOM_DIR (default /var/hr54-persist/doom):
 *
 *   frame.raw   published frames, atomic rename, never torn
 *   input       held-key bitmask, written by the backend, polled once a tick
 *   status      one line of counters, for debugging without a debugger
 *
 * frame.raw is assembled in one heap buffer and written with one write(), then
 * rename()d into place, so a reader sees either the whole previous frame or
 * the whole new one.  It is 8-bit indexed plus a palette rather than RGB
 * because that is 65 KB instead of 192 KB, and the WebKit on this box is slow
 * enough that per-pixel JavaScript is the thing to avoid.
 *
 * Frame format, all header integers little-endian so the browser does not
 * have to care about the host's byte order:
 *
 *   0    4    "HRDF"
 *   4    4    version (1)
 *   8    4    width
 *   12   4    height
 *   16   4    sequence, +1 per frame
 *   20   1024 256 palette entries, R,G,B,A
 *   1044 w*h  one palette index per pixel
 */

#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomtype.h"
#include "i_video.h"
#ifdef HR54_NATIVE_VIDEO
#include "doom_video.h"
#include "doom_input_native.h"
static int video_depth,video_selftest,native_input=1,video_seconds=1800;
static int cleanup_failed;
static int ready_fd=-1;
static void native_cleanup(void) {
    doom_input_close();
    if (doom_video_close()) cleanup_failed=1;
}
#endif

#define HRDF_MAGIC "HRDF"
#define HRDF_VER 1u
#define HDR_BYTES 1044u
#define FW DOOMGENERIC_RESX
#define FH DOOMGENERIC_RESY

/* Must match doom_input_write() in jellyfin/remote/hr54_jf.c and the KEY table
 * in doom/itv/frontend.js.  Bit 7 is the exit edge. */
#define IN_LEFT      (1u << 0)
#define IN_RIGHT     (1u << 1)
#define IN_UP        (1u << 2)
#define IN_DOWN      (1u << 3)
#define IN_FIRE      (1u << 4)
#define IN_USE       (1u << 5)
#define IN_STRAFE_L  (1u << 6)
#define IN_STRAFE_R  (1u << 7)
#define IN_EXIT      (1u << 7)

static char dir_path[512] = "/var/hr54-persist/doom";
static char frame_path[640];
static char input_path[640];
static char status_path[640];

static volatile sig_atomic_t stop_flag = 0;
static unsigned held = 0;
static unsigned long frames = 0;
static double start_ms = 0;
static int publish_failed = 0;

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static void on_term(int sig) {
    (void)sig;
    stop_flag = 1;
}

static double mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void build_path(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/%s", dir_path, name);
}

static void put_le32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

/* ------------------------------------------------------------------ */
/* key events                                                         */
/* ------------------------------------------------------------------ */

/* doomgeneric funnels every key through DG_GetKey, which i_input.c turns
 * into D_PostEvent.  There is no key array to poke, so held bits are turned
 * into real press/release edges and queued here.  Doom needs the edges: a
 * single keydown with no keyup is a stuck key, and no keydown at all is
 * nothing. */
#define QMAX 32
static struct { unsigned char key; unsigned char pressed; } queue[QMAX];
static int q_head, q_tail;

static void push_key(unsigned char key, int pressed) {
    int next = (q_tail + 1) % QMAX;
    if (next == q_head) return;   /* full: drop, same as a real driver with
                                     a full event queue */
    queue[q_tail].key = key;
    queue[q_tail].pressed = (unsigned char)(pressed ? 1 : 0);
    q_tail = next;
}

static int pop_key(unsigned char *key, int *pressed) {
    if (q_head == q_tail) return 0;
    *key = queue[q_head].key;
    *pressed = queue[q_head].pressed;
    q_head = (q_head + 1) % QMAX;
    return 1;
}

static const unsigned char bit_key[8] = {
    KEY_LEFTARROW, KEY_RIGHTARROW, KEY_UPARROW, KEY_DOWNARROW,
    KEY_RCTRL, KEY_ENTER, ',', '.'
};

static void push_edges(unsigned now_mask, unsigned was_mask) {
    int i;
    for (i = 0; i < 7; i++) {
        unsigned bit = 1u << i;
        if ((now_mask & bit) && !(was_mask & bit)) push_key(bit_key[i], 1);
        if (!(now_mask & bit) && (was_mask & bit)) push_key(bit_key[i], 0);
    }
}

/* ------------------------------------------------------------------ */
/* framebuffer                                                        */
/* ------------------------------------------------------------------ */

static unsigned char *out_buf;
static unsigned char *idx_buf;
static unsigned char pal_cache[768];

/* HR54-PATCH companion: called from i_video.c I_SetPalette. */
void DG_SetPalette(const unsigned char *playpal) {
#ifdef HR54_NATIVE_VIDEO
    extern unsigned char gammatable[5][256];
    for (int i=0;i<768;i++) pal_cache[i]=gammatable[usegamma][playpal[i]];
#else
    memcpy(pal_cache, playpal, sizeof pal_cache);
#endif
}

static int publish(const unsigned char *frame, size_t len) {
    char tmp[700];
    int fd;
    size_t off = 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", frame_path);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    for (;;) {
        ssize_t w = write(fd, frame + off, len - off);
        if (w > 0) {
            off += (size_t)w;
            if (off == len) break;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (close(fd)) { unlink(tmp); return -1; }
    if (rename(tmp, frame_path)) { unlink(tmp); return -1; }
    return 0;
}

static void write_status(void) {
    char buf[192];
    int fd;
    int n = snprintf(buf, sizeof buf,
                     "pid %d frames %lu held %u ticks %lu ms %.1f\n",
                     (int)getpid(), frames, held, (unsigned long)DG_GetTicksMs(),
                     mono_ms() - start_ms);
    fd = open(status_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (write(fd, buf, (size_t)n) != n) { /* nothing useful to do about it */ }
    close(fd);
}

/* ------------------------------------------------------------------ */
/* input                                                              */
/* ------------------------------------------------------------------ */

static unsigned read_held(void) {
    int fd = open(input_path, O_RDONLY);
    char b[32];
    ssize_t n;
    if (fd < 0) return 0;
    n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return 0;
    b[n] = 0;
    return ((unsigned)strtoul(b, NULL, 10)) & 0xffu;
}

/* ------------------------------------------------------------------ */
/* doomgeneric platform contract                                      */
/* ------------------------------------------------------------------ */

void DG_Init(void) {
    const char *env = getenv("HR54_DOOM_DIR");
    if (env && *env) snprintf(dir_path, sizeof dir_path, "%s", env);
    build_path(frame_path, sizeof frame_path, "frame.raw");
    build_path(input_path, sizeof input_path, "input");
    build_path(status_path, sizeof status_path, "status");
#ifdef HR54_NATIVE_VIDEO
    start_ms=mono_ms();
    if (native_input && doom_input_open()) {
        fprintf(stderr,"hr54-doom: native input ownership failed\n"); exit(1);
    }
    if (doom_video_open(FW,FH,video_depth)) exit(1);
    if (ready_fd>=0) { unsigned char ready=1; write(ready_fd,&ready,1); close(ready_fd); ready_fd=-1; }
    return;
#endif
    out_buf = malloc(HDR_BYTES + (size_t)FW * FH);
    idx_buf = malloc((size_t)FW * FH);
    if (!out_buf || !idx_buf) {
        fprintf(stderr, "hr54-doom: cannot allocate frame buffers\n");
        exit(1);
    }
    memcpy(out_buf, HRDF_MAGIC, 4);
    put_le32(out_buf + 4, HRDF_VER);
    put_le32(out_buf + 8, FW);
    put_le32(out_buf + 12, FH);
    /* A visible grey ramp until Doom's first I_SetPalette, so an early frame
     * is obviously a frame and not an uninitialised buffer. */
    memset(out_buf + 20, 0, 1024);
    {
        int i;
        for (i = 0; i < 256; i++) {
            unsigned char v = (unsigned char)i;
            out_buf[20 + i * 4] = v;
            out_buf[20 + i * 4 + 1] = v;
            out_buf[20 + i * 4 + 2] = v;
            out_buf[20 + i * 4 + 3] = 255;
        }
        memset(idx_buf, 0, (size_t)FW * FH);
    }
    start_ms = mono_ms();
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "hr54-doom: %dx%d frames -> %s\n", FW, FH, frame_path);
}

void DG_DrawFrame(void) {
#ifdef HR54_NATIVE_VIDEO
    if (doom_video_present(I_VideoBuffer,pal_cache)) {
        fprintf(stderr,"hr54-doom: native presentation failed\n"); exit(1);
    }
    frames++;
    if ((frames & 127)==0) {
        write_status();
        double elapsed=mono_ms()-start_ms;
        fprintf(stderr,"hr54-doom: frames=%lu elapsed_ms=%.1f average_fps=%.2f\n",
                frames,elapsed,elapsed>0?1000.0*frames/elapsed:0);
    }
    return;
#endif
    int i;
    unsigned char *pal = out_buf + 20;
    for (i = 0; i < 256; i++) {
        pal[i * 4] = pal_cache[i * 3];
        pal[i * 4 + 1] = pal_cache[i * 3 + 1];
        pal[i * 4 + 2] = pal_cache[i * 3 + 2];
        pal[i * 4 + 3] = 255;
    }
    /* I_VideoBuffer is the engine's own 8-bit indexed 320x200 framebuffer, and
     * i_video.c leaves it in place after copying it into DG_ScreenBuffer.  It
     * is published directly rather than round-tripped through the 32-bit copy
     * DoomGeneric hands the platform: that is the exact index Doom drew, so
     * the frame is pixel-identical to the original, and it is one memcpy
     * instead of 64000 conversions on a 1.3 GHz BMIPS. */
    memcpy(idx_buf, I_VideoBuffer, (size_t)FW * FH);
    put_le32(out_buf + 16, (unsigned)frames);
    if (publish(out_buf, HDR_BYTES + (size_t)FW * FH) != 0) {
        if (!publish_failed) {
            fprintf(stderr, "hr54-doom: cannot publish %s: %s\n",
                    frame_path, strerror(errno));
            publish_failed = 1;
        }
    }
    frames++;
    if ((frames & 127) == 0) write_status();
}

void DG_SleepMs(uint32_t ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) {
        if (stop_flag || errno != EINTR) break;
    }
}

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)(mono_ms() - start_ms);
}

int DG_GetKey(int *pressed, unsigned char *key) {
#ifdef HR54_NATIVE_VIDEO
    if (native_input && doom_input_pump(push_key)!=0) stop_flag=1;
#endif
    return pop_key(key, pressed);
}

void DG_SetWindowTitle(const char *title) {
    (void)title;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);
#ifdef HR54_NATIVE_VIDEO
    signal(SIGALRM,on_term);
    int have_depth=0,out=1;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--video-selftest")) video_selftest=1;
        else if (!strcmp(argv[i],"--input-file")) native_input=0;
        else if (!strcmp(argv[i],"--ready-fd") && i+1<argc) {
            char *end; long fd=strtol(argv[++i],&end,10);
            if (*end || fd<3 || fd>1023) return 2;
            ready_fd=(int)fd;
        }
        else if ((!strcmp(argv[i],"--depth") || !strcmp(argv[i],"--seconds")) && i+1<argc) {
            int is_depth=!strcmp(argv[i],"--depth");
            char *end; long number=strtol(argv[++i],&end,10);
            if (*end || number<-2147483647L || number>2147483647L) return 2;
            if (is_depth) {video_depth=(int)number;have_depth=1;}
            else {if (number<0 || number>3600) return 2;video_seconds=(int)number;}
        } else argv[out++]=argv[i];
    }
    argc=out; argv[out]=0;
    if (!have_depth) {
        fprintf(stderr,"usage: %s --depth N [--seconds N] [--video-selftest | -iwad WAD] [Doom options]\n",argv[0]);
        return 2;
    }
    if (atexit(native_cleanup)) return 1;
    alarm((unsigned)video_seconds);
    if (video_selftest) {
        DG_Init();
        while (!stop_flag) {
            if (doom_video_selftest(frames++)) return 1;
            if (native_input && doom_input_pump(push_key)!=0) stop_flag=1;
            DG_SleepMs(33);
        }
        native_cleanup(); alarm(0);
        return cleanup_failed?1:0;
    }
#endif

    if (argc < 2) {
        fprintf(stderr, "usage: %s <wad> [options]\n", argv[0]);
        return 2;
    }
    held = 0;
    doomgeneric_Create(argc, argv);
    for (;;) {
        unsigned was, now_mask;
        doomgeneric_Tick();
#ifdef HR54_NATIVE_VIDEO
        if (!native_input) {
#endif
        was = held;
        now_mask = read_held();
        held = now_mask;
        push_edges(now_mask, was);
#ifdef HR54_NATIVE_VIDEO
        }
#endif
        if (stop_flag) break;
    }
    write_status();
#ifdef HR54_NATIVE_VIDEO
    native_cleanup(); alarm(0);
    return cleanup_failed?1:0;
#else
    return 0;
#endif
}
