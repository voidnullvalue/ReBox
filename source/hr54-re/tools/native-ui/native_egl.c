#define _POSIX_C_SOURCE 200809L
#include "native_egl.h"
#include "compositor_protocol.h"
#ifdef HR54_RECEIVER
#include "receiver_runtime.h"
extern char *getenv(const char *);
#else
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#endif
enum { TEX=0xde1, RGBA=0x1908, BYTE=0x1401, FIXED=0x140c };
static uint64_t now_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC,&ts)) return 0;
    return (uint64_t)ts.tv_sec*1000000+(unsigned long)ts.tv_nsec/1000;
}
static unsigned egl_error(const Hr54EglApi *a) {
    return a->eglGetError ? a->eglGetError() : 0;
}
static int egl_result(const Hr54EglApi *a,const char *step,int rc) {
    fprintf(stderr,"native-egl: %s return=%d EGL=0x%x\n",step,rc,egl_error(a));
    return rc;
}
static int gl_result(const Hr54EglApi *a,const char *step,int verbose) {
    unsigned err=a->glGetError();
    if (err || verbose) fprintf(stderr,"native-egl: %s GL=0x%x\n",step,err);
    return err ? -1 : 0;
}
static int complete(const Hr54EglApi *a) {
    return a && a->eglGetDisplay && a->eglInitialize && a->eglGetConfigs &&
        a->eglCreateContext && a->eglCreateWindowSurface && a->eglMakeCurrent &&
        a->eglQuerySurface && a->eglDrawlistSetDepthDTV && a->eglSwapBuffers &&
        a->eglDestroySurface && a->eglDestroyContext && a->eglTerminate &&
        a->glGenTextures && a->glDeleteTextures && a->glBindTexture &&
        a->glTexImage2D && a->glTexSubImage2D && a->glPixelStorei && a->glEnable &&
        a->glEnableClientState && a->glBlendFunc && a->glMatrixMode &&
        a->glLoadIdentity && a->glScalex && a->glTranslatex && a->glViewport &&
        a->glVertexPointer && a->glTexCoordPointer && a->glDrawArrays &&
        a->glFinish && a->glGetError;
}
int hr54_egl_open(Hr54EglSurface *s,const Hr54EglApi *a,int w,int h,int depth) {
    if (!s) return -1;
    memset(s,0,sizeof(*s));
    if (!complete(a) || w<0 || h<0 || (!w != !h) || w>(int)UINT16_MAX || h>(int)UINT16_MAX ||
        (h && (size_t)w>SIZE_MAX/4/(size_t)h)) return -1;
    s->api=a;
    Hr54EglHandle config=0; int count=0, major=0, minor=0;
    fprintf(stderr,"native-egl: open host=%d depth=%d source=%dx%d (0=query)\n",
            HR54_LOCAL_DRAWLIST_HOST,depth,w,h);
    s->display=a->eglGetDisplay(HR54_LOCAL_DRAWLIST_HOST);
    fprintf(stderr,"native-egl: eglGetDisplay=%p EGL=0x%x\n",s->display,egl_error(a));
    if (!s->display) goto fail;
    if (!egl_result(a,"eglInitialize",a->eglInitialize(s->display,&major,&minor))) goto fail;
    s->initialized=1;
    fprintf(stderr,"native-egl: EGL version=%d.%d\n",major,minor);
    if (!egl_result(a,"eglGetConfigs",a->eglGetConfigs(s->display,&config,1,&count)) || count<1) goto fail;
    fprintf(stderr,"native-egl: config=%p count=%d\n",config,count);
    s->context=a->eglCreateContext(s->display,config,0,0);
    fprintf(stderr,"native-egl: eglCreateContext=%p EGL=0x%x\n",s->context,egl_error(a));
    if (!s->context) goto fail;
    s->window=a->eglCreateWindowSurface(s->display,config,0,0,0,0);
    fprintf(stderr,"native-egl: eglCreateWindowSurface(6 args)=%p EGL=0x%x\n",s->window,egl_error(a));
    if (!s->window || !egl_result(a,"eglMakeCurrent",a->eglMakeCurrent(s->display,s->window,s->window,s->context))) goto fail;
    if (!egl_result(a,"eglDrawlistSetDepthDTV",a->eglDrawlistSetDepthDTV(depth))) goto fail;
    if (!egl_result(a,"eglQuerySurface WIDTH",a->eglQuerySurface(s->display,s->window,0x3057,&s->output_width)) ||
        !egl_result(a,"eglQuerySurface HEIGHT",a->eglQuerySurface(s->display,s->window,0x3056,&s->output_height)) ||
        s->output_width<=0 || s->output_height<=0) goto fail;
    if (!w) { w=s->output_width; h=s->output_height; }
    if (w>(int)UINT16_MAX || h>(int)UINT16_MAX || (size_t)w>SIZE_MAX/4/(size_t)h) goto fail;
    s->width=w; s->height=h;
    fprintf(stderr,"native-egl: output=%dx%d texture=%dx%d depth=%d\n",
            s->output_width,s->output_height,w,h,depth);
    a->glPixelStorei(0xcf5,1);
    a->glEnableClientState(0x8074); a->glEnableClientState(0x8078);
    /* BIST's fixed-point transform and quad. */
    a->glMatrixMode(0x1700); a->glLoadIdentity();
    a->glScalex(2*65536,2*65536,65536);
    a->glTranslatex(-32768,-32768,-2*65536);
    a->glEnable(TEX); a->glEnable(0xbe2);
    a->glBlendFunc(0x302,0x303);
    if (gl_result(a,"draw state",1)) goto fail;
    a->glGenTextures(1,&s->texture);
    fprintf(stderr,"native-egl: texture ID=%u\n",s->texture);
    if (!s->texture || gl_result(a,"glGenTextures",1)) goto fail;
    a->glBindTexture(TEX,s->texture);
    a->glTexImage2D(TEX,0,RGBA,w,h,0,RGBA,BYTE,0);
    if (gl_result(a,"glTexImage2D RGBA",1)) goto fail;
    return 0;
fail:
    fprintf(stderr,"native-egl: open failed; releasing own objects\n");
    hr54_egl_close(s);
    return -1;
}
int hr54_egl_submit(void *ctx,const UiFramebuffer *f,UiRect dirty) {
    Hr54EglSurface *s=ctx;
    if (!s || !s->window || !f || !f->pixels || f->width!=s->width || f->height!=s->height ||
        f->stride<(size_t)f->width*4 || (size_t)f->height>SIZE_MAX/f->stride) return -1;
    const Hr54EglApi *a=s->api;
    uint64_t begin=now_us(),t;
    int current_rc=a->eglMakeCurrent(s->display,s->window,s->window,s->context);
    if (s->trace) egl_result(a,"eglMakeCurrent submit",current_rc);
    if (!current_rc) {
        egl_result(a,"eglMakeCurrent submit",0); return -1;
    }
    /* Match BIST ScreenUpdate: upload/draw/swap without per-frame glFinish.
     * Here glFinish flushes an additional drawlist frame before synchronizing;
     * live trials measured hundreds of milliseconds per call. The upload has
     * its own gfx_copy_pixels/dluTextureSync path. Finish remains in teardown. */
    int ow=0,oh=0;
    if (!a->eglQuerySurface(s->display,s->window,0x3057,&ow) ||
        !a->eglQuerySurface(s->display,s->window,0x3056,&oh) || ow<=0 || oh<=0) {
        egl_result(a,"query dimensions submit",0); return -1;
    }
    s->output_width=ow; s->output_height=oh;
    if (!s->submitted) dirty=(UiRect){0,0,f->width,f->height};
    if (dirty.x<0 || dirty.y<0 || dirty.w<=0 || dirty.h<=0 ||
        dirty.x>f->width-dirty.w || dirty.y>f->height-dirty.h) return -1;
    size_t row=(size_t)dirty.w*4, bytes=row*(size_t)dirty.h;
    uint8_t *packed=malloc(bytes);
    if (!packed) return -1;
    for (int y=0;y<dirty.h;y++)
        memcpy(packed+(size_t)y*row,f->pixels+(size_t)(dirty.y+y)*f->stride+(size_t)dirty.x*4,row);
    a->glBindTexture(TEX,s->texture);
    t=now_us();
    a->glTexSubImage2D(TEX,0,dirty.x,dirty.y,dirty.w,dirty.h,RGBA,BYTE,packed);
    s->upload_us+=now_us()-t;
    free(packed);
    if (gl_result(a,"glTexSubImage2D",!s->submitted)) return -1;
    /* Vendor glClear(0x4000) emits dlSurfaceClear for this context's surface.
     * Otherwise dlSurfaceFlush preserves earlier commands beneath the new
     * alpha-blended quad: work grows with frame count and hiding is not a
     * replacement. This opt-in is for the native shell's complete frames. */
    if (s->replace_frame) {
        if (!a->glClear) return -1;
        a->glViewport(0,0,ow,oh);
        a->glClear(0x4000);
        if (gl_result(a,"glClear own frame",!s->submitted)) return -1;
    }
    int vx=0,vy=0,vw=ow,vh=oh;
    if (s->aspect_width>0 && s->aspect_height>0) {
        if ((int64_t)ow*s->aspect_height>(int64_t)oh*s->aspect_width)
            vw=(int)((int64_t)oh*s->aspect_width/s->aspect_height);
        else vh=(int)((int64_t)ow*s->aspect_height/s->aspect_width);
        vx=(ow-vw)/2; vy=(oh-vh)/2;
    }
    a->glViewport(vx,vy,vw,vh);
    static const int vertices[]={0,0,65536,0,0,65536,65536,65536};
    static const int coords[]={0,65536,65536,65536,0,0,65536,0};
    a->glVertexPointer(2,FIXED,0,vertices);
    a->glTexCoordPointer(2,FIXED,0,coords);
    a->glDrawArrays(5,0,4);
    if (gl_result(a,"glDrawArrays",!s->submitted)) return -1;
    t=now_us();
    int rc=a->eglSwapBuffers(s->display,s->window);
    s->swap_us+=now_us()-t;
    if (s->trace || !s->submitted || !rc) egl_result(a,"eglSwapBuffers",rc);
    if (!rc) return -1;
    s->submitted=1; s->frames++;
    t=now_us()-begin;
    if (t>250000 && getenv("HR54_INPUT_TRACE"))
        fprintf(stderr,"render-trace: slow-submit us=%llu frames=%lu upload-total-us=%llu swap-total-us=%llu\n",
            (unsigned long long)t,s->frames,(unsigned long long)s->upload_us,(unsigned long long)s->swap_us);
    if (t>s->max_present_us) s->max_present_us=t;
    return 0;
}
int hr54_egl_close(Hr54EglSurface *s) {
    int result=0;
    if (!s || !s->api) return 0;
    const Hr54EglApi *a=s->api;
    if (s->submitted && s->window && s->texture) {
        UiFramebuffer blank;
        if (!ui_fb_init(&blank,s->width,s->height)) {
            int rc=hr54_egl_submit(s,&blank,blank.dirty);
            fprintf(stderr,"native-egl: transparent final frame=%d\n",rc);
            if (rc) result=-1;
            ui_fb_free(&blank);
        } else result=-1;
    }
    if (s->context && s->window) {
        if (a->eglMakeCurrent(s->display,s->window,s->window,s->context)) {
            uint64_t begin=now_us(); a->glFinish(); s->finish_us+=now_us()-begin;
            if (gl_result(a,"glFinish teardown",1)) result=-1;
            if (s->texture) {
                a->glDeleteTextures(1,&s->texture);
                if (gl_result(a,"glDeleteTextures",1)) result=-1;
            }
        } else { egl_result(a,"eglMakeCurrent teardown",0); result=-1; }
    }
    if (s->initialized && !egl_result(a,"eglMakeCurrent unbind",a->eglMakeCurrent(s->display,0,0,0))) result=-1;
    if (s->window && !egl_result(a,"eglDestroySurface",a->eglDestroySurface(s->display,s->window))) result=-1;
    if (s->context && !egl_result(a,"eglDestroyContext",a->eglDestroyContext(s->display,s->context))) result=-1;
    if (s->initialized && !egl_result(a,"eglTerminate client",a->eglTerminate(s->display))) result=-1;
    fprintf(stderr,"native-egl: close=%d frames=%lu upload_us=%llu swap_us=%llu finish_us=%llu max_present_us=%llu\n",
            result,s->frames,(unsigned long long)s->upload_us,(unsigned long long)s->swap_us,
            (unsigned long long)s->finish_us,(unsigned long long)s->max_present_us);
    memset(s,0,sizeof(*s));
    return result;
}
