#ifndef JPQ_FONT_H
#define JPQ_FONT_H
/* Packed alpha font atlases. Overpass is pre-rasterised at build time by
 * tools/build_font_atlas.py; the renderer has no font runtime dependency. */
#include "jpq_view.h"
#include "../../../tools/native-ui/framebuffer.h"

typedef struct {
    short px, py, w, h, bx, by, adv;
} JpqGlyph;

typedef struct {
    const unsigned char *bits;
    int width, height, ascent, line;
    const JpqGlyph *glyphs;
    int count, first;
} JpqFontAtlas;

extern const JpqFontAtlas jpq_font_body, jpq_font_title, jpq_font_small, jpq_font_tiny;

/* Advance width of one glyph in atlas pixels. */
int jpq_text_width(const JpqFontAtlas *, const char *text);
/* Draw text with the pen origin at (x, baseline). Returns the advance. */
int jpq_text_draw(UiFramebuffer *, const JpqFontAtlas *, int x, int baseline,
                  const char *text, UiColor color);
/* Draw one glyph given its code point; unknown codes draw nothing. */
const JpqGlyph *jpq_glyph(const JpqFontAtlas *, int code);
#endif
