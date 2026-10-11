#include "jpq_render.h"
#include "jpq_font.h"
#include "jpq_qr.h"
#include "jpq_remote.h"
#ifdef HR54_RECEIVER
#include "receiver_runtime.h"
#else
#include <string.h>
#endif

/* ---- palette ---------------------------------------------------------- */
static const UiColor COBALT = {10, 24, 74, 255};
static const UiColor COBALT_HI = {22, 63, 176, 255};
static const UiColor GOLD = {245, 197, 66, 255};
static const UiColor GOLD_DIM = {176, 138, 38, 255};
static const UiColor INK = {238, 243, 255, 255};
static const UiColor DIM = {150, 170, 210, 255};
static const UiColor GOOD = {53, 208, 127, 255};
static const UiColor BAD = {255, 90, 106, 255};
static const UiColor DARK = {6, 12, 34, 255};
static const UiColor PALETTE[6] = {
    {16, 57, 160, 255}, {245, 197, 66, 255}, {220, 84, 68, 255},
    {49, 132, 104, 255}, {177, 122, 205, 255}, {224, 170, 60, 255}
};

static int streq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Map the service's named podium colour to an RGB triple. Unknown/absent names
 * fall back to the id-stable palette, so a player always has one distinct
 * colour on every play scene. */
static UiColor named_color(const char *n, int fallback) {
    if (streq(n, "cobalt")) return PALETTE[0];
    if (streq(n, "gold")) return PALETTE[1];
    if (streq(n, "crimson")) return PALETTE[2];
    if (streq(n, "jade")) return PALETTE[3];
    if (streq(n, "violet")) return PALETTE[4];
    if (streq(n, "amber")) return PALETTE[5];
    return PALETTE[((fallback % 6) + 6) % 6];
}

static UiColor player_color(const JpqView *v, int i) {
    if (v && i >= 0 && i < v->player_count) {
        const JvPlayer *p = &v->players[i];
        int fb = p->id ? (int)(p->id % 6) : i;
        if (p->color[0]) return named_color(p->color, fb);
        return PALETTE[((fb % 6) + 6) % 6];
    }
    return PALETTE[((i % 6) + 6) % 6];
}

/* Scoreboard tween state, updated once per render call. */
static int32_t g_prev_score[JPQ_VIEW_PLAYERS];
static int g_tween_pm = 1000;

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ---- background ------------------------------------------------------- */
static void background(UiFramebuffer *fb, uint64_t anim_ms) {
    int shift = (int)((anim_ms / 60) % 64);
    for (int y = 0; y < fb->height; y++) {
        int t = (y * 200) / (fb->height > 0 ? fb->height : 1);
        for (int x = 0; x < fb->width; x++) {
            int radial = (x - fb->width / 2) * (x - fb->width / 2) * 120 / (fb->width * fb->width + 1);
            int glow = (x / 24 + y / 24 + shift) % 8 == 0 ? 4 : 0;
            UiColor c;
            c.r = (uint8_t)clamp255(COBALT.r + (COBALT_HI.r - COBALT.r) * (200 - t) / 200 - radial + glow);
            c.g = (uint8_t)clamp255(COBALT.g + (COBALT_HI.g - COBALT.g) * (200 - t) / 200 - radial + glow);
            c.b = (uint8_t)clamp255(COBALT.b + (COBALT_HI.b - COBALT.b) * (200 - t) / 200 - radial / 2 + glow);
            c.a = 255;
            ui_fb_rect(fb, (UiRect){x, y, 1, 1}, c);
        }
    }
}

static void vignette(UiFramebuffer *fb) {
    int w = fb->width, h = fb->height;
    for (int y = 0; y < h; y++) {
        int dy = (y < h / 2 ? h / 2 - y : y - h / 2) * 2;
        for (int x = 0; x < w; x++) {
            int dx = (x < w / 2 ? w / 2 - x : x - w / 2) * 2;
            int d = (dx * dx + dy * dy);
            if (d < (w * w + h * h) / 3) continue;
            int a = (d * 90) / (w * w + h * h);
            ui_fb_rect(fb, (UiRect){x, y, 1, 1}, (UiColor){0, 0, 0, (uint8_t)clamp255(a)});
        }
    }
}

static void double_border(UiFramebuffer *fb, int x, int y, int w, int h) {
    ui_fb_rect(fb, (UiRect){x, y, w, 2}, GOLD);
    ui_fb_rect(fb, (UiRect){x, y + h - 2, w, 2}, GOLD);
    ui_fb_rect(fb, (UiRect){x, y, 2, h}, GOLD);
    ui_fb_rect(fb, (UiRect){x + w - 2, y, 2, h}, GOLD);
    ui_fb_rect(fb, (UiRect){x + 5, y + 5, w - 10, 1}, GOLD_DIM);
    ui_fb_rect(fb, (UiRect){x + 5, y + h - 6, w - 10, 1}, GOLD_DIM);
    ui_fb_rect(fb, (UiRect){x + 5, y + 5, 1, h - 10}, GOLD_DIM);
    ui_fb_rect(fb, (UiRect){x + w - 6, y + 5, 1, h - 10}, GOLD_DIM);
}

static int span_width(const JpqFontAtlas *f, const char *s, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) {
        const JpqGlyph *g = jpq_glyph(f, (unsigned char)s[i]);
        w += g ? g->adv : f->ascent / 3 + 2;
    }
    return w;
}

/* Count the centred word-wrap lines a string needs at a font, capped at
 * maxlines+1 so an overlong string is detected without unbounded work. */
static int count_wrap_lines(const JpqFontAtlas *f, int maxw, const char *s, int maxlines) {
    int lines = 0, have = 0, cur_w = 0;
    const char *word = s;
    while (*word && lines <= maxlines) {
        const char *e = word;
        while (*e && *e != ' ') e++;
        int ww = span_width(f, word, (int)(e - word));
        int sp = have ? span_width(f, " ", 1) : 0;
        if (have && cur_w + sp + ww > maxw) { lines++; have = 0; cur_w = 0; continue; }
        cur_w += sp + ww;
        have = 1;
        word = e;
        while (*word == ' ') word++;
    }
    if (have) lines++;
    return lines;
}

/* Every whitespace-delimited word must fit on one line by itself; otherwise we
 * drop a font size rather than break the word or spill into the neighbour. */
static int all_words_fit(const JpqFontAtlas *f, int maxw, const char *s) {
    const char *word = s;
    while (word && *word) {
        while (*word == ' ') word++;
        const char *e = word;
        while (*e && *e != ' ') e++;
        if (span_width(f, word, (int)(e - word)) > maxw) return 0;
        word = e;
    }
    return 1;
}

/* Pick the largest atlas whose measured wrap still fits the box with whole
 * words; fall back to the body atlas so long clues/answers shrink instead of
 * overlapping. */
static const JpqFontAtlas *fit_font(int maxw, const char *s, int maxlines) {
    if (s && *s) {
        if (all_words_fit(&jpq_font_title, maxw, s) &&
            count_wrap_lines(&jpq_font_title, maxw, s, maxlines) <= maxlines)
            return &jpq_font_title;
        if (all_words_fit(&jpq_font_body, maxw, s) &&
            count_wrap_lines(&jpq_font_body, maxw, s, maxlines) <= maxlines)
            return &jpq_font_body;
        if (all_words_fit(&jpq_font_small, maxw, s) &&
            count_wrap_lines(&jpq_font_small, maxw, s, maxlines) <= maxlines)
            return &jpq_font_small;
    }
    return &jpq_font_tiny;
}

/* Copy s into out, replacing only a word that cannot fit maxw at font f with a
 * bounded ellipsis; whole words are never split mid-character. This keeps a
 * genuine monster category word from fragmenting across the column while the
 * full name is still available in the wide clue header. */
static void ellipsize_words(const JpqFontAtlas *f, int maxw, const char *s, char *out, size_t cap) {
    size_t o = 0;
    const char *word = s;
    while (*word && o + 1 < cap) {
        while (*word == ' ') word++;
        if (!*word) break;
        const char *e = word;
        while (*e && *e != ' ') e++;
        int wl = (int)(e - word);
        if (o) out[o++] = ' ';
        if (span_width(f, word, wl) <= maxw) {
            for (int i = 0; i < wl && o + 1 < cap; i++) out[o++] = word[i];
        } else {
            static const char dots[3] = {'.', '.', '.'};
            int denom = span_width(f, dots, 3);
            int cur = 0, n = 0;
            if (denom > maxw) denom = maxw;               /* pathological: clamp */
            while (n < wl && o + 1 < cap) {
                int adv = span_width(f, word + n, 1);
                if (cur + adv > maxw - denom) break;
                out[o++] = word[n++];
                cur += adv;
            }
            for (int i = 0; i < 3 && o + 1 < cap; i++) out[o++] = dots[i];
        }
        word = e;
    }
    out[o] = 0;
}

/* ---- text ------------------------------------------------------------- */
static void text_center(UiFramebuffer *fb, const JpqFontAtlas *f, int cx, int base,
                        const char *s, UiColor c) {
    int w = jpq_text_width(f, s);
    jpq_text_draw(fb, f, cx - w / 2, base, s, c);
}

/* Greedy, measured word wrap; draws each line centred. The running width is
 * tracked from real glyph advances (never an approximate wl*8) and the line is
 * NUL-terminated before it is measured/drawn, so no uninitialised read. Every
 * line is bounded so text never overlaps its neighbours. Returns the baseline
 * after the last line. */
static int wrap_center(UiFramebuffer *fb, const JpqFontAtlas *f, int cx, int base,
                       int maxw, const char *s, UiColor c, int maxlines) {
    char line[JPQ_TEXT_MAX + 2];
    int line_h = f->line + 4;
    int lines = 0, n = 0, cur_w = 0;
    line[0] = 0;
    const char *word = s;
    while (*word && lines < maxlines) {
        const char *e = word;
        while (*e && *e != ' ') e++;
        int wl = (int)(e - word);
        int ww = span_width(f, word, wl);
        int sp = n ? span_width(f, " ", 1) : 0;
        if (n && cur_w + sp + ww > maxw) {
            line[n] = 0;
            text_center(fb, f, cx, base + lines * line_h, line, c);
            lines++;
            n = 0; cur_w = 0;
            continue;
        }
        if (!n && ww > maxw) {
            /* A single word wider than the box (no font shrinks enough): split it
             * by character so it still cannot overlap its neighbour. */
            for (int i = 0; i < wl && n < (int)sizeof line - 1; i++) {
                int adv = span_width(f, word + i, 1);
                if (cur_w + adv > maxw && n) {
                    line[n] = 0;
                    text_center(fb, f, cx, base + lines * line_h, line, c);
                    lines++;
                    n = 0; cur_w = 0;
                }
                line[n++] = word[i];
                cur_w += adv;
            }
            word = e;
            while (*word == ' ') word++;
            continue;
        }
        if (n) { line[n++] = ' '; cur_w += sp; }
        for (int i = 0; i < wl && n < (int)sizeof line - 1; i++) line[n++] = word[i];
        cur_w += ww;
        word = e;
        while (*word == ' ') word++;
    }
    if (n && lines < maxlines) {
        line[n] = 0;
        text_center(fb, f, cx, base + lines * line_h, line, c);
        lines++;
    }
    return base + lines * line_h;
}

/* ---- numbers ---------------------------------------------------------- */
static void i32str(int v, char *out) {
    char tmp[16];
    int n = 0, neg = v < 0;
    unsigned u = neg ? (unsigned)(-(long)v) : (unsigned)v;
    if (!u) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    int o = 0;
    if (neg) out[o++] = '-';
    while (n) out[o++] = tmp[--n];
    out[o] = 0;
}

/* Dollar value for board tiles / wagers (always carries the currency sign). */
static void money(int v, char *out) {
    char tmp[16];
    int n = 0, neg = v < 0;
    unsigned u = neg ? (unsigned)(-(long)v) : (unsigned)v;
    if (!u) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    int o = 0;
    if (neg) out[o++] = '-';
    out[o++] = '$';
    while (n) out[o++] = tmp[--n];
    out[o] = 0;
}

/* Vertically centre a wrapped text block inside a box so long category names
 * and clues never spill into a neighbour. */
static void text_block(UiFramebuffer *fb, const JpqFontAtlas *f, int cx, int top, int boxh,
                       int maxw, const char *s, UiColor c, int maxlines) {
    int n = count_wrap_lines(f, maxw, s, maxlines);
    int lh = f->line + 2;
    int base = top + (boxh - n * lh) / 2 + f->ascent;
    if (base < top + f->ascent) base = top + f->ascent;
    wrap_center(fb, f, cx, base, maxw, s, c, maxlines);
}

/* ---- glossy tiles ----------------------------------------------------- */
static void glossy(UiFramebuffer *fb, int x, int y, int w, int h, UiColor base,
                   int selected, int reveal_pm) {
    if (reveal_pm <= 0) return;
    int rh = h * reveal_pm / 1000;
    if (rh < 1) return;
    for (int i = 0; i < rh; i++) {
        int t = i * 255 / (rh ? rh : 1);
        UiColor c = base;
        c.r = (uint8_t)clamp255(base.r + 40 * (255 - t) / 255);
        c.g = (uint8_t)clamp255(base.g + 40 * (255 - t) / 255);
        c.b = (uint8_t)clamp255(base.b + 30 * (255 - t) / 255);
        c.a = 255;
        ui_fb_rect(fb, (UiRect){x, y + i, w, 1}, c);
    }
    /* gloss band */
    ui_fb_rect(fb, (UiRect){x + 3, y + 2, w - 6, (rh > 8 ? rh / 4 : 1)}, (UiColor){255, 255, 255, 26});
    ui_fb_rect(fb, (UiRect){x, y, w, 1}, GOLD_DIM);
    ui_fb_rect(fb, (UiRect){x, y + rh - 1, w, 1}, (UiColor){0, 0, 0, 90});
    if (selected) {
        ui_fb_rect(fb, (UiRect){x - 2, y - 2, w + 4, 2}, GOLD);
        ui_fb_rect(fb, (UiRect){x - 2, y + rh, w + 4, 2}, GOLD);
        ui_fb_rect(fb, (UiRect){x - 2, y - 2, 2, rh + 4}, GOLD);
        ui_fb_rect(fb, (UiRect){x + w, y - 2, 2, rh + 4}, GOLD);
    }
}

static int ease(uint64_t elapsed, uint64_t dur, int pm_max) {
    if (dur == 0 || elapsed >= dur) return pm_max;
    return (int)(elapsed * (uint64_t)pm_max / dur);
}

/* ---- QR --------------------------------------------------------------- */
void jpq_qr_payload(char *out, size_t cap, const JpqMetadata *m) {
    size_t o = 0;
    const char *url = (m && m->url[0]) ? m->url : "http://127.0.0.1/";
    for (size_t i = 0; url[i] && o + 1 < cap; i++) out[o++] = url[i];
    if (m && m->join_pin[0]) {
        const char *frag = "#pin=";
        for (size_t i = 0; frag[i] && o + 1 < cap; i++) out[o++] = frag[i];
        for (size_t i = 0; m->join_pin[i] && o + 1 < cap; i++) out[o++] = m->join_pin[i];
    }
    out[o] = 0;
}

static void draw_qr(UiFramebuffer *fb, int x, int y, int box, const char *payload) {
    JpqQr qr;
    if (jpq_qr_encode(payload, -1, &qr)) return;
    int quiet = 2, n = qr.size + quiet * 2;
    int scale = box / n;
    if (scale < 1) scale = 1;
    int side = n * scale;
    int ox = x + (box - side) / 2, oy = y + (box - side) / 2;
    ui_fb_rect(fb, (UiRect){ox, oy, side, side}, (UiColor){255, 255, 255, 255});
    for (int r = 0; r < qr.size; r++) for (int c = 0; c < qr.size; c++) {
        if (!qr.module[r][c]) continue;
        ui_fb_rect(fb, (UiRect){ox + (c + quiet) * scale, oy + (r + quiet) * scale, scale, scale}, DARK);
    }
}

/* Podium the TV should highlight: during a live clue it is the player who
 * actually buzzed (or the daily-double wagerer), never the scoreboard leader. */
int jpq_render_focus_podium(const JpqView *v) {
    if (!v || !v->valid || v->player_count <= 0) return -1;
    if (v->phase == JV_CLUE || v->phase == JV_TIEBREAK_CLUE) {
        if (v->buzzed >= 0 && v->buzzed < v->player_count) return v->buzzed;
    } else if (v->phase == JV_DD_CLUE) {
        if (v->dd_selector >= 0 && v->dd_selector < v->player_count) return v->dd_selector;
    }
    return v->leader;
}

/* ---- scoreboard ------------------------------------------------------- */
static void scoreboard(UiFramebuffer *fb, const JpqView *v, int y, int show_ready) {
    int n = v->player_count;
    if (n <= 0) return;
    int gap = 6, total = fb->width - 40;
    int cw = (total - gap * (n - 1)) / n;
    if (cw > 150) cw = 150;
    int x0 = (fb->width - (cw * n + gap * (n - 1))) / 2;
    int focus = jpq_render_focus_podium(v);
    char buf[32];
    for (int i = 0; i < n; i++) {
        const JvPlayer *p = &v->players[i];
        int x = x0 + i * (cw + gap);
        UiColor pc = player_color(v, i);
        int lead = (i == focus);
        ui_fb_rect(fb, (UiRect){x, y, cw, 44}, (UiColor){4, 12, 40, 210});
        ui_fb_rect(fb, (UiRect){x, y, cw, 3}, pc);
        if (lead) ui_fb_rect(fb, (UiRect){x, y, cw, 44}, (UiColor){GOLD.r, GOLD.g, GOLD.b, 0});
        if (lead) double_border(fb, x, y, cw, 44);
        text_center(fb, &jpq_font_body, x + cw / 2, y + 18, p->name, INK);
        int32_t shown = p->score;
        if (i < JPQ_VIEW_PLAYERS && g_tween_pm < 1000) shown = g_prev_score[i] + (p->score - g_prev_score[i]) * g_tween_pm / 1000;
        i32str(shown, buf);
        text_center(fb, &jpq_font_body, x + cw / 2, y + 38, buf, lead ? GOLD : INK);
        if (show_ready) {
            ui_fb_rect(fb, (UiRect){x + 4, y + 4, 6, 6}, p->ready ? GOOD : DIM);
            if (!p->connected) ui_fb_rect(fb, (UiRect){x, y, cw, 44}, (UiColor){0, 0, 0, 120});
        }
    }
}

static const char *round_title(uint8_t round) {
    switch (round) {
    case JV_ROUND_DJ: return "DOUBLE JEOPARDY!";
    case JV_ROUND_FINAL: return "FINAL JEOPARDY!";
    case JV_ROUND_TIEBREAK: return "SUDDEN DEATH";
    default: return "JEOPARDY!";
    }
}

static const char *phase_label(uint8_t phase) {
    switch (phase) {
    case JV_BOARD: return "SELECT A CLUE";
    case JV_CLUE: return "BUZZ IN";
    case JV_DD_WAGER: return "DAILY DOUBLE WAGER";
    case JV_DD_CLUE: return "DAILY DOUBLE";
    case JV_FINAL_WAGER: return "MAKE YOUR WAGER";
    case JV_FINAL_CLUE: return "WRITE YOUR ANSWER";
    case JV_FINAL_JUDGE: return "JUDGING";
    case JV_RESULTS: return "FINAL SCORES";
    case JV_REVEAL: return "ANSWER";
    default: return "LOBBY";
    }
}

/* ---- scenes ----------------------------------------------------------- */
static void scene_lobby(UiFramebuffer *fb, const JpqView *v, const JpqMetadata *m, uint64_t now) {
    char payload[300];
    jpq_qr_payload(payload, sizeof payload, m);
    text_center(fb, &jpq_font_title, fb->width / 2, 58, "JEOPARDY!", GOLD);
    text_center(fb, &jpq_font_body, fb->width / 2, 80, "JOIN ON YOUR PHONE", DIM);
    draw_qr(fb, 40, 110, 190, payload);
    text_center(fb, &jpq_font_body, 135, 320, "SCAN TO JOIN", INK);
    /* PIN block */
    char pin[48];
    int o = 0;
    const char *lbl = "JOIN PIN";
    for (int i = 0; lbl[i]; i++) pin[o++] = lbl[i];
    pin[o++] = ' ';
    if (m && m->join_pin[0]) for (int i = 0; m->join_pin[i]; i++) pin[o++] = m->join_pin[i];
    else { const char *d = "----"; for (int i = 0; d[i]; i++) pin[o++] = d[i]; }
    pin[o] = 0;
    text_center(fb, &jpq_font_title, fb->width * 3 / 4, 150, pin, GOLD);
    const char *url = (m && m->url[0]) ? m->url : "waiting for LAN address...";
    wrap_center(fb, &jpq_font_body, fb->width * 3 / 4, 185, fb->width / 2 - 70, url, INK, 3);
    text_center(fb, &jpq_font_body, fb->width * 3 / 4, 250, phase_label(v->phase), DIM);
    scoreboard(fb, v, 300, 1);
    int ready = 0;
    for (int i = 0; i < v->player_count; i++) ready += v->players[i].ready;
    if (v->player_count == 0)
        text_center(fb, &jpq_font_body, fb->width / 2, fb->height - 40, "WAITING FOR PLAYERS", DIM);
    else if (ready < v->player_count)
        text_center(fb, &jpq_font_body, fb->width / 2, fb->height - 40, "WAITING FOR READY", DIM);
    else
        text_center(fb, &jpq_font_title, fb->width / 2, fb->height - 36, "PRESS OK TO START", GOLD);
    (void)now;
}

/* Minimum on-screen safe margin: keeps borders, categories, scoreboards and
 * help text inside the framebuffer with room to spare. */
#define JPQ_MARGIN 24

static int bottom_score_y(const UiFramebuffer *fb);

static void scene_board(UiFramebuffer *fb, const JpqView *v, uint64_t now, uint64_t anim) {
    text_center(fb, &jpq_font_title, fb->width / 2, 46, round_title(v->round), GOLD);
    int score_y = bottom_score_y(fb);
    /* A dedicated hint band above the podium row: the hint baseline sits clear
     * of the podium border and the board stops above the hint glyphs, so the
     * help line can never overlap the top scoreboard row. */
    int hint_base = score_y - 10;
    int top = 62;
    int gap = 6;
    int cat_h = 54;
    int board_bottom = hint_base - jpq_font_tiny.ascent - 6;
    int board_h = board_bottom - top;
    if (board_h < cat_h + gap * JPQ_VIEW_ROWS + 18 * JPQ_VIEW_ROWS) board_h = cat_h + gap * JPQ_VIEW_ROWS + 18 * JPQ_VIEW_ROWS;
    int cellw = (fb->width - 2 * JPQ_MARGIN - gap * (JPQ_VIEW_COLS - 1)) / JPQ_VIEW_COLS;
    int cellh = (board_h - cat_h - gap * JPQ_VIEW_ROWS) / JPQ_VIEW_ROWS;
    if (cellh < 18) cellh = 18;
    char buf[32];
    for (int c = 0; c < JPQ_VIEW_COLS; c++) {
        int x = JPQ_MARGIN + c * (cellw + gap);
        int reveal = ease(anim, 240 + c * 120, 1000);
        glossy(fb, x, top, cellw, cat_h, COBALT_HI, 0, reveal);
        if (reveal > 700) {
            /* Full category words, wrapped at word boundaries with an adaptive
             * font: never a mid-word slash like "LITER/TURE". The 12px atlas
             * keeps the longest real words whole; a genuine monster word is
             * ellipsised rather than split, and shown in full in the clue
             * header once the cell is opened. */
            const char *nm = v->categories[c].name[0] ? v->categories[c].name : "-";
            const JpqFontAtlas *cf = fit_font(cellw - 8, nm, 3);
            char catbuf[JPQ_LABEL_MAX + 8];
            ellipsize_words(cf, cellw - 8, nm, catbuf, sizeof catbuf);
            text_block(fb, cf, x + cellw / 2, top + 2, cat_h - 4, cellw - 8, catbuf, INK, 3);
        }
        for (int r = 0; r < JPQ_VIEW_ROWS; r++) {
            int y = top + cat_h + gap + r * (cellh + gap);
            int used = v->categories[c].used[r];
            int selected = (v->phase == JV_BOARD && v->clue.present && v->clue.col == c && v->clue.row == r);
            int reveal = ease(anim, 400 + (c * JPQ_VIEW_ROWS + r) * 30, 1000);
            UiColor base = used ? (UiColor){24, 34, 66, 255} : COBALT_HI;
            glossy(fb, x, y, cellw, cellh, base, selected, reveal);
            if (reveal > 600 && !used) {
                money(v->categories[c].value[r], buf);
                const JpqFontAtlas *vf = jpq_text_width(&jpq_font_title, buf) <= cellw - 8 ? &jpq_font_title : &jpq_font_body;
                text_center(fb, vf, x + cellw / 2, y + cellh / 2 + vf->ascent / 2, buf, GOLD);
            }
        }
    }
    scoreboard(fb, v, score_y, 0);
    text_center(fb, &jpq_font_tiny, fb->width / 2, hint_base,
                "D-PAD MOVE  OK SELECT  RED WRONG  INFO SETTINGS", DIM);
    (void)now;
}

static int bottom_score_y(const UiFramebuffer *fb) { return fb->height - JPQ_MARGIN - 44; }

static void timer_bar(UiFramebuffer *fb, const JpqView *v, uint64_t now, int y) {
    if (v->timer_kind == JV_TIMER_NONE) return;
    uint64_t rem = jpq_view_timer_remaining(v, now);
    uint32_t total = 10000;
    switch (v->timer_kind) {
    case JV_TIMER_READ: total = 10000; break;
    case JV_TIMER_BUZZ: total = 10000; break;
    case JV_TIMER_ANSWER: total = 10000; break;
    default: total = 15000; break;
    }
    int frac = total ? (int)(rem * 1000 / total) : 0;
    if (frac > 1000) frac = 1000;
    int barw = fb->width - 2 * JPQ_MARGIN - 24;
    int bx = (fb->width - barw) / 2;
    ui_fb_rect(fb, (UiRect){bx, y, barw, 12}, (UiColor){4, 12, 40, 200});
    UiColor c = frac < 250 ? BAD : frac < 550 ? GOLD : GOOD;
    ui_fb_rect(fb, (UiRect){bx, y, barw * frac / 1000, 12}, c);
    char sec[16];
    i32str((int)((rem + 999) / 1000), sec);
    text_center(fb, &jpq_font_body, fb->width / 2, y - 6, sec, INK);
}

static void scene_clue(UiFramebuffer *fb, const JpqView *v, uint64_t now, uint64_t anim) {
    const JvClue *q = &v->clue;
    if (q->daily_double) {
        int flash = (anim < 800) ? ((anim / 100) % 2) : 0;
        if (flash) ui_fb_rect(fb, (UiRect){0, 0, fb->width, fb->height}, (UiColor){245, 197, 66, 70});
    }
    int score_y = bottom_score_y(fb);
    int bar_y = score_y - 24;
    int textw = fb->width - 2 * JPQ_MARGIN;
    int cat_bottom;
    if (q->daily_double) {
        text_center(fb, &jpq_font_title, fb->width / 2, 46, "DAILY DOUBLE", GOLD);
        cat_bottom = 46 + 20;
    } else {
        /* Category and clue occupy measured, non-overlapping zones: a two-line
         * category pushes the clue text down instead of printing over it. */
        const char *cat = q->category[0] ? q->category : "";
        const JpqFontAtlas *cf = fit_font(textw, cat, 2);
        int cl = count_wrap_lines(cf, textw, cat, 2);
        if (cl > 2) cl = 2;
        if (cl < 1) cl = 1;
        int band = cl * cf->line + 4;
        text_block(fb, cf, fb->width / 2, 14, band, textw, cat, INK, 2);
        cat_bottom = 14 + band;
    }
    if (v->phase == JV_DD_WAGER) {
        text_center(fb, &jpq_font_title, fb->width / 2, 128, "MAKE YOUR WAGER", GOLD);
        char buf[32];
        int sel = v->dd_selector >= 0 ? v->dd_selector : (v->selector >= 0 ? v->selector : 0);
        if (sel >= v->player_count) sel = 0;
        if (v->player_count) {
            text_center(fb, &jpq_font_body, fb->width / 2, 175, v->players[sel].name, player_color(v, sel));
            if (v->players[sel].has_wager) { money(v->players[sel].wager, buf); text_center(fb, &jpq_font_title, fb->width / 2, 225, buf, GOLD); }
        }
        scoreboard(fb, v, score_y, 0);
        timer_bar(fb, v, now, bar_y);
        return;
    }
    if (v->phase == JV_DD_CLUE && !q->has_text) {
        text_center(fb, &jpq_font_title, fb->width / 2, 190, "WAGER LOCKED", GOLD);
        scoreboard(fb, v, score_y, 0);
        timer_bar(fb, v, now, bar_y);
        return;
    }
    const char *text = q->has_text ? q->text : "CLUE HIDDEN";
    const JpqFontAtlas *qf = fit_font(textw, text, 5);
    int base = cat_bottom + 12 + qf->ascent;
    int end = wrap_center(fb, qf, fb->width / 2, base, textw, text, INK, 5);
    int player = (v->phase == JV_DD_CLUE) ? v->dd_selector : v->buzzed;
    if (player >= 0 && player < v->player_count) {
        char who[80];
        int n = 0;
        const char *pre = (v->phase == JV_DD_CLUE) ? "ANSWERING: " : "BUZZED: ";
        for (int i = 0; pre[i]; i++) who[n++] = pre[i];
        for (int i = 0; v->players[player].name[i] && n < (int)sizeof who - 1; i++) who[n++] = v->players[player].name[i];
        who[n] = 0;
        text_center(fb, &jpq_font_title, fb->width / 2, end + 30, who, player_color(v, player));
        end += 30;
    }
    if (q->has_response && (v->phase == JV_REVEAL || v->phase == JV_RESULTS)) {
        char resp[JPQ_TEXT_MAX + 16];
        int n = 0;
        const char *pre = "ANSWER: ";
        for (int i = 0; pre[i]; i++) resp[n++] = pre[i];
        for (int i = 0; q->response[i] && n < (int)sizeof resp - 1; i++) resp[n++] = q->response[i];
        resp[n] = 0;
        const JpqFontAtlas *rf = fit_font(textw, resp, 3);
        int ry = end + 26;
        if (ry + rf->line * 3 > bar_y - 4) ry = bar_y - 4 - rf->line * 3;
        if (ry < end + 4) ry = end + 4;
        wrap_center(fb, rf, fb->width / 2, ry, textw, resp, GOOD, 3);
    }
    scoreboard(fb, v, score_y, 0);
    timer_bar(fb, v, now, bar_y);
    (void)anim;
}

/* A row of per-contestant verdict chips for the sequential Final judging: the
 * already-judged keep their real correct/incorrect mark and the pending ones
 * stay dim, so the TV shows the true order, not an inferred one. */
static void judge_chips(UiFramebuffer *fb, const JpqView *v, int y) {
    int n = 0;
    for (int i = 0; i < v->player_count; i++) if (v->players[i].eligible) n++;
    if (!n) return;
    int gap = 8, cw = (fb->width - 2 * JPQ_MARGIN - gap * (n - 1)) / n;
    if (cw > 150) cw = 150;
    int total = cw * n + gap * (n - 1);
    int x = (fb->width - total) / 2;
    for (int i = 0; i < v->player_count; i++) {
        if (!v->players[i].eligible) continue;
        UiColor mark = v->players[i].judged ? (v->players[i].correct ? GOOD : BAD) : DIM;
        ui_fb_rect(fb, (UiRect){x, y, cw, 18}, (UiColor){4, 12, 40, 200});
        ui_fb_rect(fb, (UiRect){x, y, 4, 18}, player_color(v, i));
        char line[40];
        int o = 0;
        for (int k = 0; v->players[i].name[k] && o < (int)sizeof line - 3; k++) line[o++] = v->players[i].name[k];
        line[o++] = ' ';
        if (!v->players[i].judged) line[o++] = '-';
        else line[o++] = v->players[i].correct ? '+' : 'x';
        line[o] = 0;
        text_center(fb, &jpq_font_body, x + cw / 2, y + 14, line, mark);
        x += cw + gap;
    }
}

static void scene_final(UiFramebuffer *fb, const JpqView *v, uint64_t now, uint64_t anim) {
    int score_y = bottom_score_y(fb);
    int bar_y = score_y - 24;
    int textw = fb->width - 2 * JPQ_MARGIN;
    text_center(fb, &jpq_font_title, fb->width / 2, 40, "FINAL JEOPARDY!", GOLD);
    /* Category and clue occupy measured, non-overlapping zones: the category
     * block never reaches into the clue text (WORLD CAPITALS above the clue,
     * never on top of it). */
    int cat_top = 50, cat_h = 36;
    if (v->final.category[0]) {
        const JpqFontAtlas *cf = fit_font(textw, v->final.category, 2);
        text_block(fb, cf, fb->width / 2, cat_top, cat_h, textw, v->final.category, INK, 2);
    }
    int clue_top = cat_top + cat_h + 10;
    if (v->phase == JV_FINAL_WAGER) {
        text_center(fb, &jpq_font_title, fb->width / 2, clue_top + 24, "WAGERS", GOLD);
        int cw = (fb->width - 2 * JPQ_MARGIN) / (v->player_count ? v->player_count : 1);
        for (int i = 0; i < v->player_count; i++) {
            char buf[32];
            if (!v->players[i].eligible) continue;
            if (v->players[i].has_wager) money(v->players[i].wager, buf);
            else { buf[0] = '-'; buf[1] = '-'; buf[2] = 0; }
            text_center(fb, &jpq_font_title, JPQ_MARGIN + i * cw + cw / 2, clue_top + 62, buf, v->players[i].has_wager ? GOLD : DIM);
        }
        scoreboard(fb, v, score_y, 0);
        timer_bar(fb, v, now, bar_y);
        return;
    }
    const char *text = v->final.has_text ? v->final.text : "CLUE HIDDEN";
    const JpqFontAtlas *qf = fit_font(textw, text, 3);
    int end = wrap_center(fb, qf, fb->width / 2, clue_top + qf->ascent, textw, text, INK, 3);
    if (v->phase == JV_FINAL_JUDGE) {
        int chips_y = score_y - 30;
        int detail_bottom = chips_y - 6;
        int cur = v->judge_cursor;
        int y = end + 16;
        if (cur >= 0 && cur < v->player_count) {
            char line[80];
            int n = 0;
            const char *nm = v->players[cur].name;
            for (int i = 0; nm[i] && n < (int)sizeof line - 3; i++) line[n++] = nm[i];
            if (v->players[cur].wager_set) { char b[32]; money(v->players[cur].wager, b); line[n++] = ' '; for (int i = 0; b[i] && n < (int)sizeof line - 1; i++) line[n++] = b[i]; }
            line[n] = 0;
            text_center(fb, &jpq_font_title, fb->width / 2, y + jpq_font_title.ascent, line, player_color(v, cur));
            y += jpq_font_title.line + 2;
            /* the current contestant's typed (public) answer */
            if (v->players[cur].has_answer && v->players[cur].answer[0]) {
                const JpqFontAtlas *af = fit_font(textw, v->players[cur].answer, 2);
                int al = count_wrap_lines(af, textw, v->players[cur].answer, 1);
                if (al > 2) al = 2;
                int abh = al * af->line + 6;
                text_block(fb, af, fb->width / 2, y, abh, textw, v->players[cur].answer, INK, 2);
                y += abh + 2;
            }
        }
        /* The correct response is shown to the adjudicating host only once all
         * private answers are in (this phase): a compact, measured block below
         * the answer that never overlaps the wager/answer rows. */
        if (v->final.has_response && v->final.response[0]) {
            char ans[JPQ_TEXT_MAX + 16];
            int k = 0;
            const char *pre = "ANSWER: ";
            for (int i = 0; pre[i]; i++) ans[k++] = pre[i];
            for (int i = 0; v->final.response[i] && k < (int)sizeof ans - 1; i++) ans[k++] = v->final.response[i];
            ans[k] = 0;
            const JpqFontAtlas *rf = fit_font(textw, ans, 2);
            int rl = count_wrap_lines(rf, textw, ans, 1);
            if (rl > 2) rl = 2;
            int rbh = rl * rf->line + 6;
            int rtop = y;
            if (rtop + rbh > detail_bottom) rtop = detail_bottom - rbh;
            if (rtop < end + 4) rtop = end + 4;
            text_block(fb, rf, fb->width / 2, rtop, rbh, textw, ans, GOOD, 2);
        }
        judge_chips(fb, v, chips_y);
    } else if (v->final.has_response) {
        char ans[JPQ_TEXT_MAX + 16];
        int k = 0;
        const char *pre = "ANSWER: ";
        for (int i = 0; pre[i]; i++) ans[k++] = pre[i];
        for (int i = 0; v->final.response[i] && k < (int)sizeof ans - 1; i++) ans[k++] = v->final.response[i];
        ans[k] = 0;
        const JpqFontAtlas *rf = fit_font(textw, ans, 3);
        int ry = end + 26;
        if (ry + rf->line * 3 > bar_y - 4) ry = bar_y - 4 - rf->line * 3;
        if (ry < end + 4) ry = end + 4;
        wrap_center(fb, rf, fb->width / 2, ry, textw, ans, GOOD, 3);
    }
    scoreboard(fb, v, score_y, 0);
    timer_bar(fb, v, now, bar_y);
    (void)anim;
}

static void confetti(UiFramebuffer *fb, uint64_t now) {
    for (int i = 0; i < 120; i++) {
        int x = (i * 97 + (int)(now / 8)) % fb->width;
        int y = (i * 53 + (int)(now / 5)) % fb->height;
        UiColor c = PALETTE[i % 6];
        ui_fb_rect(fb, (UiRect){x, y, 3, 6}, c);
    }
}

static void scene_results(UiFramebuffer *fb, const JpqView *v, uint64_t now, uint64_t anim) {
    text_center(fb, &jpq_font_title, fb->width / 2, 56, "FINAL SCORES", GOLD);
    if (v->tie) confetti(fb, now);
    /* rank players by score (selection sort over a copy) */
    int idx[JPQ_VIEW_PLAYERS];
    for (int i = 0; i < v->player_count; i++) idx[i] = i;
    for (int i = 0; i < v->player_count; i++)
        for (int j = i + 1; j < v->player_count; j++)
            if (v->players[idx[j]].score > v->players[idx[i]].score) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    int n = v->player_count;
    int gap = 8, cw = n ? (fb->width - 60 - gap * (n - 1)) / n : 0;
    if (cw > 170) cw = 170;
    int x0 = (fb->width - (cw * n + gap * (n - 1))) / 2;
    char buf[32];
    for (int rank = 0; rank < n; rank++) {
        int i = idx[rank];
        const JvPlayer *p = &v->players[i];
        int x = x0 + rank * (cw + gap);
        int h = 90 + (rank == 0 ? 30 : rank == 1 ? 16 : 0);
        int y = 150 + (rank == 0 ? 0 : 30) - (rank == 0 ? 0 : rank == 1 ? 16 : 0);
        int grow = ease(anim, rank * 150, 1000);
        int hh = h * grow / 1000;
        UiColor pc = player_color(v, i);
        ui_fb_rect(fb, (UiRect){x, y + h - hh, cw, hh}, (UiColor){4, 12, 40, 220});
        ui_fb_rect(fb, (UiRect){x, y + h - hh, cw, 4}, pc);
        if (rank == 0 && grow > 900) double_border(fb, x, y, cw, h);
        text_center(fb, &jpq_font_body, x + cw / 2, y + h - hh + 22, p->name, INK);
        i32str(p->score, buf);
        text_center(fb, &jpq_font_title, x + cw / 2, y + h - 18, buf, rank == 0 ? GOLD : INK);
    }
    if (n) {
        int wi = (v->leader >= 0 && v->leader < v->player_count) ? v->leader : idx[0];
        const char *win = v->tie ? "TIE GAME!" : v->players[wi].name;
        char line[64];
        int o = 0;
        const char *pre = v->tie ? "" : "WINNER: ";
        for (int i = 0; pre[i]; i++) line[o++] = pre[i];
        for (int i = 0; win[i] && o < (int)sizeof line - 1; i++) line[o++] = win[i];
        line[o] = 0;
        /* The winner name is lightened bright green (high contrast against the
         * cobalt) inside a banner bordered in the winner's own podium colour,
         * so it reads clearly as the result and not as just another label. */
        UiColor border = (wi >= 0 && wi < v->player_count) ? player_color(v, wi) : GOLD;
        UiColor bright = v->tie ? GOLD : (UiColor){150, 255, 190, 255};
        int tw = jpq_text_width(&jpq_font_title, line);
        int bw = tw + 40, bh = 42;
        int bx = fb->width / 2 - bw / 2, by = 92;
        ui_fb_rect(fb, (UiRect){bx - 4, by - 4, bw + 8, bh + 8}, (UiColor){border.r, border.g, border.b, 70});
        ui_fb_rect(fb, (UiRect){bx, by, bw, 2}, border);
        ui_fb_rect(fb, (UiRect){bx, by + bh - 2, bw, 2}, border);
        ui_fb_rect(fb, (UiRect){bx, by, 2, bh}, border);
        ui_fb_rect(fb, (UiRect){bx + bw - 2, by, 2, bh}, border);
        text_center(fb, &jpq_font_title, fb->width / 2,
                    by + (bh - (jpq_font_title.ascent + (jpq_font_title.line - jpq_font_title.ascent))) / 2 + jpq_font_title.ascent,
                    line, bright);
    }
    if (v->tiebreak && v->tiebreak_winner >= 0 && v->tiebreak_winner < v->player_count) {
        char line[64];
        int o = 0;
        const char *pre = "SUDDEN DEATH: ";
        for (int i = 0; pre[i]; i++) line[o++] = pre[i];
        for (int i = 0; v->players[v->tiebreak_winner].name[i] && o < (int)sizeof line - 1; i++) line[o++] = v->players[v->tiebreak_winner].name[i];
        line[o] = 0;
        text_center(fb, &jpq_font_body, fb->width / 2, fb->height - JPQ_MARGIN, line, GOLD);
    }
}

static void paused_overlay(UiFramebuffer *fb) {
    ui_fb_rect(fb, (UiRect){0, 0, fb->width, fb->height}, (UiColor){3, 8, 24, 150});
    text_center(fb, &jpq_font_title, fb->width / 2, fb->height / 2 - 30, "PAUSED", GOLD);
    text_center(fb, &jpq_font_body, fb->width / 2, fb->height / 2 + 10, "OK RESUME  INFO SETTINGS  STOP QUIT", INK);
    text_center(fb, &jpq_font_body, fb->width / 2, fb->height / 2 + 36, "BACK RESUME", DIM);
}

void jpq_render(UiFramebuffer *fb, const JpqView *v, const JpqMetadata *m,
                uint64_t now_ms, uint64_t anim_ms, const int32_t *prev_score) {
    for (int i = 0; i < JPQ_VIEW_PLAYERS; i++)
        g_prev_score[i] = prev_score ? prev_score[i] : (v ? v->players[i].score : 0);
    g_tween_pm = (int)ease(now_ms >= anim_ms ? now_ms - anim_ms : 0, 650, 1000);
    background(fb, anim_ms);
    vignette(fb);
    if (!v || !v->valid) {
        text_center(fb, &jpq_font_title, fb->width / 2, fb->height / 2, "JEOPARDY!", GOLD);
        text_center(fb, &jpq_font_body, fb->width / 2, fb->height / 2 + 30, "WAITING FOR SERVICE", DIM);
        double_border(fb, 8, 8, fb->width - 16, fb->height - 16);
        return;
    }
    switch (v->phase) {
    case JV_LOBBY: scene_lobby(fb, v, m, now_ms); break;
    case JV_BOARD: scene_board(fb, v, now_ms, anim_ms); break;
    case JV_DD_WAGER: case JV_DD_CLUE: case JV_TIEBREAK_CLUE:
        scene_clue(fb, v, now_ms, anim_ms); break;
    case JV_FINAL_WAGER: case JV_FINAL_CLUE: case JV_FINAL_JUDGE:
        scene_final(fb, v, now_ms, anim_ms); break;
    case JV_RESULTS: scene_results(fb, v, now_ms, anim_ms); break;
    default: scene_clue(fb, v, now_ms, anim_ms); break;
    }
    if (v->paused && v->phase != JV_LOBBY) paused_overlay(fb);
    double_border(fb, 8, 8, fb->width - 16, fb->height - 16);
}

void jpq_render_status(UiFramebuffer *fb, const char *status) {
    if (!status || !*status) return;
    int w = fb->width - 2 * JPQ_MARGIN;
    int x = JPQ_MARGIN, y = JPQ_MARGIN;
    ui_fb_rect(fb, (UiRect){x, y, w, 22}, (UiColor){4, 12, 40, 220});
    double_border(fb, x, y, w, 22);
    text_center(fb, &jpq_font_body, fb->width / 2, y + 16, status, GOLD);
}

void jpq_render_overlay(UiFramebuffer *fb, const JpqRemote *r, const JpqMetadata *m, const JpqView *v) {
    (void)v;
    if (!r || r->menu == JRM_NONE) return;
    int w = 380, x = (fb->width - w) / 2, y = 120;
    int h = (r->menu == JRM_SETTINGS) ? 312 : 210;
    ui_fb_rect(fb, (UiRect){x, y, w, h}, (UiColor){6, 14, 44, 235});
    double_border(fb, x, y, w, h);
    text_center(fb, &jpq_font_body, x + w / 2, y + 26, jpq_remote_menu_label(r), GOLD);
    if (r->menu == JRM_HOST) {
        char line[48];
        int o = 0;
        const char *pre = "HOST PIN: ";
        for (int i = 0; pre[i]; i++) line[o++] = pre[i];
        if (m && m->host_pin[0]) for (int i = 0; m->host_pin[i]; i++) line[o++] = m->host_pin[i];
        else { const char *d = "------"; for (int i = 0; d[i]; i++) line[o++] = d[i]; }
        line[o] = 0;
        text_center(fb, &jpq_font_title, x + w / 2, y + 90, line, GOLD);
        text_center(fb, &jpq_font_body, x + w / 2, y + 130, "KEEP PRIVATE - HOST PHONE ONLY", DIM);
        return;
    }
    if (r->menu == JRM_TIMERS) {
        char buf[32];
        const char *labels[3] = {"READ", "BUZZ", "ANSWER"};
        int32_t vals[3] = {r->t_read, r->t_buzz, r->t_answer};
        for (int i = 0; i < 3; i++) {
            char line[48];
            int o = 0;
            for (int k = 0; labels[i][k]; k++) line[o++] = labels[i][k];
            line[o++] = ' '; line[o++] = ' ';
            i32str(vals[i] / 1000, buf);
            for (int k = 0; buf[k]; k++) line[o++] = buf[k];
            line[o++] = 's'; line[o] = 0;
            text_center(fb, &jpq_font_body, x + w / 2, y + 70 + i * 34, line, i == r->timer_field ? GOLD : INK);
        }
        return;
    }
    if (r->menu == JRM_SCORE) {
        char line[64];
        int o = 0;
        int idx = (r->score_player >= 0 && r->score_player < v->player_count) ? r->score_player : 0;
        const char *nm = v->player_count ? v->players[idx].name : "-";
        for (int i = 0; nm[i] && o < (int)sizeof line - 1; i++) line[o++] = nm[i];
        line[o++] = ':'; line[o++] = ' ';
        char b[16];
        i32str(r->score_value, b);
        for (int i = 0; b[i] && o < (int)sizeof line - 1; i++) line[o++] = b[i];
        line[o] = 0;
        text_center(fb, &jpq_font_title, x + w / 2, y + 100, line, GOLD);
        text_center(fb, &jpq_font_body, x + w / 2, y + 140, "OK APPLY  BACK CANCEL", DIM);
        return;
    }
    const char *items[8];
    int n = 0;
    if (r->menu == JRM_PAUSE) { items[0] = "RESUME"; items[1] = "RESTART"; items[2] = "QUIT"; n = 3; }
    else if (r->menu == JRM_SETTINGS) { items[0] = "RESUME"; items[1] = "RESTART"; items[2] = "QUIT"; items[3] = "SCORE EDIT"; items[4] = "HOST CONNECTION"; items[5] = "TIMERS"; items[6] = "REVEAL ANSWER"; items[7] = "PASS"; n = 8; }
    else {
        const char *hint = (r->menu == JRM_CONFIRM_RESTART) ? "OK CONFIRM  BACK CANCEL" : "STOP AGAIN TO CONFIRM  BACK CANCEL";
        text_center(fb, &jpq_font_body, x + w / 2, y + 100, hint, INK);
        return;
    }
    for (int i = 0; i < n; i++)
        text_center(fb, &jpq_font_body, x + w / 2, y + 52 + i * 26, items[i], i == r->cursor ? GOLD : INK);
}
