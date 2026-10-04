#ifndef HR54_NATIVE_EGL_H
#define HR54_NATIVE_EGL_H
#include "surface.h"
/* Receiver vendor ABI, deliberately independent of standard EGL headers.
 * eglCreateWindowSurface reads its attribute list from MIPS o32 argument SIX.
 * The middle three arguments are passed as zero, exactly as BIST does. Their
 * declared int types are inferred from callers; they are unused by this path. */
typedef void *Hr54EglHandle;
typedef struct {
    Hr54EglHandle (*eglGetDisplay)(int);
    int (*eglInitialize)(Hr54EglHandle,int *,int *);
    int (*eglGetConfigs)(Hr54EglHandle,Hr54EglHandle *,int,int *);
    Hr54EglHandle (*eglCreateContext)(Hr54EglHandle,Hr54EglHandle,Hr54EglHandle,const int *);
    Hr54EglHandle (*eglCreateWindowSurface)(Hr54EglHandle,Hr54EglHandle,int,int,int,const int *);
    int (*eglMakeCurrent)(Hr54EglHandle,Hr54EglHandle,Hr54EglHandle,Hr54EglHandle);
    int (*eglQuerySurface)(Hr54EglHandle,Hr54EglHandle,int,int *);
    int (*eglDrawlistSetDepthDTV)(int);
    int (*eglSwapBuffers)(Hr54EglHandle,Hr54EglHandle);
    int (*eglDestroySurface)(Hr54EglHandle,Hr54EglHandle);
    int (*eglDestroyContext)(Hr54EglHandle,Hr54EglHandle);
    int (*eglTerminate)(Hr54EglHandle);
    void (*glGenTextures)(int,unsigned *);
    void (*glDeleteTextures)(int,const unsigned *);
    void (*glBindTexture)(unsigned,unsigned);
    void (*glTexImage2D)(unsigned,int,int,int,int,int,unsigned,unsigned,const void *);
    void (*glTexSubImage2D)(unsigned,int,int,int,int,int,unsigned,unsigned,const void *);
    void (*glPixelStorei)(unsigned,int);
    void (*glEnable)(unsigned);
    void (*glEnableClientState)(unsigned);
    void (*glBlendFunc)(unsigned,unsigned);
    void (*glMatrixMode)(unsigned);
    void (*glLoadIdentity)(void);
    void (*glScalex)(int,int,int);
    void (*glTranslatex)(int,int,int);
    void (*glViewport)(int,int,int,int);
    void (*glVertexPointer)(int,unsigned,int,const void *);
    void (*glTexCoordPointer)(int,unsigned,int,const void *);
    void (*glDrawArrays)(unsigned,int,int);
    void (*glFinish)(void);
    unsigned (*glGetError)(void);
    unsigned (*eglGetError)(void); /* Optional for host fakes. */
    void (*glClear)(unsigned);
} Hr54EglApi;
typedef struct {
    const Hr54EglApi *api;
    Hr54EglHandle display, context, window;
    unsigned texture;
    int width,height,output_width,output_height,initialized,submitted;
    unsigned long frames;
    uint64_t upload_us,swap_us,finish_us,max_present_us;
    int trace; /* Per-frame diagnostics for bounded tests; off for games. */
    int aspect_width,aspect_height; /* 0 = fill viewport; otherwise fit. */
    int replace_frame; /* Clear own retained commands before a complete quad. */
} Hr54EglSurface;
/* Explicit depth is a policy input. (0,0) raster size selects queried output. */
int hr54_egl_open(Hr54EglSurface *,const Hr54EglApi *,int,int,int);
int hr54_egl_submit(void *,const UiFramebuffer *,UiRect);
int hr54_egl_close(Hr54EglSurface *);
/* Defined in receiver_bindings.c, excluded from default host executables. */
extern const Hr54EglApi hr54_receiver_egl_api;
#endif
