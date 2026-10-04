#include "doom_video.h"
#include "native_egl.h"
#include <stdio.h>
#include <string.h>
static Hr54EglSurface surface;
static UiFramebuffer rgba;
int doom_video_open(int w,int h,int depth) {
    if (hr54_egl_open(&surface,&hr54_receiver_egl_api,w,h,depth)) return -1;
    surface.aspect_width=4; surface.aspect_height=3;
    if (ui_fb_init(&rgba,w,h)) { hr54_egl_close(&surface); return -1; }
    fprintf(stderr,"doom-video: indexed %dx%d -> RGBA -> native EGL; aspect=4:3 depth=%d\n",w,h,depth);
    return 0;
}
static int present(void) {
    return hr54_egl_submit(&surface,&rgba,(UiRect){0,0,rgba.width,rgba.height});
}
int doom_video_present(const unsigned char *indices,const unsigned char *palette) {
    if (!rgba.pixels || !indices || !palette) return -1;
    for (int y=0;y<rgba.height;y++) for (int x=0;x<rgba.width;x++) {
        unsigned i=indices[y*rgba.width+x];
        unsigned char *p=rgba.pixels+(size_t)y*rgba.stride+(size_t)x*4;
        p[0]=palette[3*i]; p[1]=palette[3*i+1]; p[2]=palette[3*i+2]; p[3]=255;
    }
    return present();
}
int doom_video_selftest(unsigned long seq) {
    UiColor color=(seq/60)%2 ? (UiColor){0,180,0,255} : (UiColor){180,0,0,255};
    ui_fb_clear(&rgba,color);
    for (int y=0;y<rgba.height;y+=20) for (int x=0;x<rgba.width;x+=20)
        if (((x/20+y/20)&1)==0) ui_fb_rect(&rgba,(UiRect){x,y,20,20},(UiColor){30,30,30,255});
    ui_fb_rect(&rgba,(UiRect){(int)(seq*3%(rgba.width-24)),80,24,40},(UiColor){255,255,0,255});
    char label[64]; snprintf(label,sizeof(label),"NATIVE EGL FRAME %lu",seq);
    ui_fb_text(&rgba,10,10,label,1,(UiColor){255,255,255,255});
    return present();
}
int doom_video_close(void) {
    int result=hr54_egl_close(&surface); ui_fb_free(&rgba); return result;
}
