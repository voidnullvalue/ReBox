#define _POSIX_C_SOURCE 200809L
#include "native_egl.h"
#ifdef HR54_RECEIVER
#include "receiver_runtime.h"
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#endif
static volatile sig_atomic_t stopping;
static void stop(int sig) { stopping=sig; }
static int number(const char *s,int *n) {
    char *end; long v=strtol(s,&end,10);
    if (!*s || *end || v<INT_MIN || v>INT_MAX) return -1;
    *n=(int)v; return 0;
}
int main(int argc,char **argv) {
    int depth=0,have_depth=0,seconds=10,hold=0,result=1;
    Hr54EglSurface surface={0}; UiFramebuffer frame={0};
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--depth") && i+1<argc) {
            if (number(argv[++i],&depth)) goto usage;
            have_depth=1;
        } else if (!strcmp(argv[i],"--seconds") && i+1<argc) {
            if (number(argv[++i],&seconds) || seconds<1 || seconds>3600) goto usage;
        } else if (!strcmp(argv[i],"--hold")) {
            hold=1;
        } else goto usage;
    }
    if (!have_depth) goto usage;
    signal(SIGTERM,stop); signal(SIGINT,stop); signal(SIGALRM,stop);
    alarm((unsigned)seconds); /* includes initialization and the final hold */
    fprintf(stderr,"smoke: pid=%d depth=%d timeout=%ds; one window/texture\n",getpid(),depth,seconds);
    if (hr54_egl_open(&surface,&hr54_receiver_egl_api,0,0,depth)) goto done;
    surface.trace=1;
    if (ui_fb_init(&frame,surface.width,surface.height)) goto done;
    for (int sequence=0;(hold || sequence<4) && !stopping;sequence++) {
        int stage=sequence%4;
        if (stage==0 || stage==2) {
            ui_fb_clear(&frame,stage==0 ? (UiColor){255,0,0,255} : (UiColor){0,255,0,255});
        } else {
            ui_fb_clear(&frame,(UiColor){0,0,0,255});
            for (int y=0;y<frame.height;y+=48) for (int x=0;x<frame.width;x+=48)
                if (((x/48+y/48)&1)==0)
                    ui_fb_rect(&frame,(UiRect){x,y,48,48},(UiColor){255,255,255,255});
        }
        int rc=hr54_egl_submit(&surface,&frame,(UiRect){0,0,frame.width,frame.height});
        fprintf(stderr,"smoke: stage=%d pattern=%s submit=%d output=%dx%d depth=%d\n",stage,
                stage==0?"RED":stage==2?"GREEN":"CHECKERBOARD",rc,
                surface.output_width,surface.output_height,depth);
        if (rc) goto done;
        /* Wait in short slices so signals are handled outside vendor calls. */
        for (int tick=0;tick<(hold?30:seconds*10/4) && !stopping;tick++) {
            struct timespec wait={0,100000000}; nanosleep(&wait,0);
        }
    }
    result=0;
done:
    fprintf(stderr,"smoke: shutdown reason=%d\n",(int)stopping);
    if (hr54_egl_close(&surface)) result=1;
    ui_fb_free(&frame);
    alarm(0);
    fprintf(stderr,"smoke: cleanup complete exit=%d\n",result);
    return result;
usage:
    fprintf(stderr,"usage: %s --depth N [--seconds 1..3600] [--hold]\n",argv[0]);
    return 2;
}
