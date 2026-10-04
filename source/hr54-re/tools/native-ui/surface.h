#ifndef HR54_UI_SURFACE_H
#define HR54_UI_SURFACE_H
#include "framebuffer.h"
/* Separable event-loop presentation interface. Only an accepted submission
 * clears dirty state; failures leave pixels available for a later retry. */
typedef struct {
    void *context;
    int (*submit)(void *, const UiFramebuffer *, UiRect);
} UiSurface;
int ui_present(UiSurface *, UiFramebuffer *);
#endif
