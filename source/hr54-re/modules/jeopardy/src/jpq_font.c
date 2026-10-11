#include "jpq_font.h"
#include "jpq_font_data.h"

const JpqGlyph *jpq_glyph(const JpqFontAtlas *f, int code) {
    int i = code - f->first;
    if (i < 0 || i >= f->count) return 0;
    return &f->glyphs[i];
}

int jpq_text_width(const JpqFontAtlas *f, const char *text) {
    int w = 0;
    for (; text && *text; text++) {
        const JpqGlyph *g = jpq_glyph(f, (unsigned char)*text);
        w += g ? g->adv : f->ascent / 3 + 2;
    }
    return w;
}

/* Straight-alpha coverage blit; no scaling, no filtering, no allocation. */
int jpq_text_draw(UiFramebuffer *fb, const JpqFontAtlas *f, int x, int baseline,
                  const char *text, UiColor color) {
    int pen = x;
    for (; text && *text; text++) {
        const JpqGlyph *g = jpq_glyph(f, (unsigned char)*text);
        if (!g) {
            pen += f->ascent / 3 + 2;
            continue;
        }
        int ox = pen + g->bx, oy = baseline + g->by;
        for (int gy = 0; gy < g->h; gy++) {
            int py = oy + gy;
            if (py < 0 || py >= fb->height) continue;
            const unsigned char *row = f->bits + (size_t)(g->py + gy) * f->width + g->px;
            for (int gx = 0; gx < g->w; gx++) {
                unsigned a = row[gx];
                if (!a) continue;
                int px = ox + gx;
                if (px < 0 || px >= fb->width) continue;
                UiColor c = color;
                c.a = (unsigned char)((color.a * a + 127) / 255);
                ui_fb_rect(fb, (UiRect){px, py, 1, 1}, c);
            }
        }
        pen += g->adv;
    }
    return pen - x;
}
