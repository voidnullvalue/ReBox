/* Receiver link-time bindings; never include desktop EGL headers. */
#include "native_egl.h"
#define DECLARE(name) extern __typeof__(*(((Hr54EglApi *)0)->name)) name
DECLARE(eglGetDisplay); DECLARE(eglInitialize); DECLARE(eglGetConfigs);
DECLARE(eglCreateContext); DECLARE(eglCreateWindowSurface); DECLARE(eglMakeCurrent);
DECLARE(eglQuerySurface); DECLARE(eglDrawlistSetDepthDTV); DECLARE(eglSwapBuffers);
DECLARE(eglDestroySurface); DECLARE(eglDestroyContext); DECLARE(eglTerminate);
DECLARE(glGenTextures); DECLARE(glDeleteTextures); DECLARE(glBindTexture);
DECLARE(glTexImage2D); DECLARE(glTexSubImage2D); DECLARE(glPixelStorei);
DECLARE(glEnable); DECLARE(glEnableClientState); DECLARE(glBlendFunc);
DECLARE(glMatrixMode); DECLARE(glLoadIdentity); DECLARE(glScalex);
DECLARE(glTranslatex); DECLARE(glViewport); DECLARE(glVertexPointer);
DECLARE(glTexCoordPointer); DECLARE(glDrawArrays); DECLARE(glFinish); DECLARE(glGetError);
DECLARE(eglGetError);
DECLARE(glClear);
const Hr54EglApi hr54_receiver_egl_api={
    eglGetDisplay,eglInitialize,eglGetConfigs,eglCreateContext,eglCreateWindowSurface,
    eglMakeCurrent,eglQuerySurface,eglDrawlistSetDepthDTV,eglSwapBuffers,
    eglDestroySurface,eglDestroyContext,eglTerminate,glGenTextures,glDeleteTextures,
    glBindTexture,glTexImage2D,glTexSubImage2D,glPixelStorei,glEnable,
    glEnableClientState,glBlendFunc,glMatrixMode,glLoadIdentity,glScalex,glTranslatex,
    glViewport,glVertexPointer,glTexCoordPointer,glDrawArrays,glFinish,glGetError,eglGetError,glClear
};
