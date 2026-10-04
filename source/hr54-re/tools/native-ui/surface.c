#include "surface.h"
int ui_present(UiSurface *s,UiFramebuffer *f) {
    if (!f->dirty.w) return 0;
    if (!s || !s->submit || s->submit(s->context,f,f->dirty)) return -1;
    ui_fb_clean(f);
    return 0;
}
