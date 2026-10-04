#ifndef HR54_FRAMEBUFFER_H
#define HR54_FRAMEBUFFER_H
#include <stddef.h>
#include <stdint.h>
/* Canonical upload representation: straight-alpha R,G,B,A bytes, top row first.
 * This is NOT a native grmem layout or an endian-dependent uint32_t color. */
typedef struct { uint8_t r, g, b, a; } UiColor;
typedef struct { int x, y, w, h; } UiRect;
typedef struct {
    uint8_t *pixels;
    int width, height;
    size_t stride;
    UiRect dirty;
} UiFramebuffer;
int ui_fb_init(UiFramebuffer *, int, int);
void ui_fb_free(UiFramebuffer *);
void ui_fb_damage(UiFramebuffer *, UiRect);
void ui_fb_clean(UiFramebuffer *);
void ui_fb_clear(UiFramebuffer *, UiColor);
void ui_fb_rect(UiFramebuffer *, UiRect, UiColor);
void ui_fb_blit(UiFramebuffer *, int, int, const uint8_t *, int, int, size_t);
void ui_fb_text(UiFramebuffer *, int, int, const char *, int, UiColor);
#endif
