#include "framebuffer.h"
#ifdef HR54_RECEIVER
#include "receiver_runtime.h"
#else
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#endif

static UiRect clip(const UiFramebuffer *f, UiRect r) {
    int64_t x0=r.x, y0=r.y, x1=x0+r.w, y1=y0+r.h;
    if (r.w<=0 || r.h<=0) return (UiRect){0,0,0,0};
    if (x0<0) x0=0;
    if (y0<0) y0=0;
    if (x1>f->width) x1=f->width;
    if (y1>f->height) y1=f->height;
    if (x1<=x0 || y1<=y0) return (UiRect){0,0,0,0};
    return (UiRect){(int)x0,(int)y0,(int)(x1-x0),(int)(y1-y0)};
}
int ui_fb_init(UiFramebuffer *f, int w, int h) {
    memset(f,0,sizeof(*f));
    if (w<=0 || h<=0 || (size_t)w>SIZE_MAX/4/(size_t)h) return -1;
    f->pixels=calloc((size_t)w*(size_t)h,4);
    if (!f->pixels) return -1;
    f->width=w; f->height=h; f->stride=(size_t)w*4;
    ui_fb_damage(f,(UiRect){0,0,w,h});
    return 0;
}
void ui_fb_free(UiFramebuffer *f) { free(f->pixels); memset(f,0,sizeof(*f)); }
void ui_fb_clean(UiFramebuffer *f) { f->dirty=(UiRect){0,0,0,0}; }
void ui_fb_damage(UiFramebuffer *f, UiRect r) {
    r=clip(f,r);
    if (!r.w) return;
    if (!f->dirty.w) { f->dirty=r; return; }
    int x=r.x<f->dirty.x?r.x:f->dirty.x;
    int y=r.y<f->dirty.y?r.y:f->dirty.y;
    int right=r.x+r.w>f->dirty.x+f->dirty.w?r.x+r.w:f->dirty.x+f->dirty.w;
    int bottom=r.y+r.h>f->dirty.y+f->dirty.h?r.y+r.h:f->dirty.y+f->dirty.h;
    f->dirty=(UiRect){x,y,right-x,bottom-y};
}
/* Source-over, retaining straight alpha even when the destination is translucent. */
static void blend(uint8_t *d, UiColor c) {
    unsigned sa=c.a, da=d[3], inv=255-sa;
    unsigned den=sa*255+da*inv;
    if (!sa) return;
    const uint8_t rgb[3]={c.r,c.g,c.b};
    for (int i=0;i<3;i++)
        d[i]=(uint8_t)((rgb[i]*sa*255+d[i]*da*inv+den/2)/den);
    d[3]=(uint8_t)((den+127)/255);
}
void ui_fb_clear(UiFramebuffer *f, UiColor c) {
    for (int y=0;y<f->height;y++) for (int x=0;x<f->width;x++) {
        uint8_t *p=f->pixels+(size_t)y*f->stride+(size_t)x*4;
        p[0]=c.r; p[1]=c.g; p[2]=c.b; p[3]=c.a;
    }
    ui_fb_damage(f,(UiRect){0,0,f->width,f->height});
}
void ui_fb_rect(UiFramebuffer *f, UiRect r, UiColor c) {
    r=clip(f,r);
    for (int y=r.y;y<r.y+r.h;y++) for (int x=r.x;x<r.x+r.w;x++)
        blend(f->pixels+(size_t)y*f->stride+(size_t)x*4,c);
    if (c.a) ui_fb_damage(f,r);
}
void ui_fb_blit(UiFramebuffer *f, int x, int y, const uint8_t *src,
                int w, int h, size_t stride) {
    if (!src || w<=0 || h<=0 || (size_t)w>SIZE_MAX/4 ||
        stride<(size_t)w*4 || (size_t)h>SIZE_MAX/stride) return;
    UiRect r=clip(f,(UiRect){x,y,w,h});
    for (int dy=r.y;dy<r.y+r.h;dy++) for (int dx=r.x;dx<r.x+r.w;dx++) {
        const uint8_t *s=src+(size_t)((int64_t)dy-y)*stride+(size_t)((int64_t)dx-x)*4;
        blend(f->pixels+(size_t)dy*f->stride+(size_t)dx*4,(UiColor){s[0],s[1],s[2],s[3]});
    }
    ui_fb_damage(f,r);
}
/* Original, deliberately small 5x7 font. Lowercase folds to uppercase.
 * No firmware font assets, font library, or host font dependency. */
static const struct { char c; uint8_t rows[7]; } glyphs[]={
 {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
 {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
 {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
 {'G',{14,17,16,23,17,17,15}}, {'H',{17,17,17,31,17,17,17}},
 {'I',{14,4,4,4,4,4,14}}, {'J',{7,2,2,2,18,18,12}},
 {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
 {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,25,21,19,19,17}},
 {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
 {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
 {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}},
 {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
 {'W',{17,17,17,21,21,27,17}}, {'X',{17,17,10,4,10,17,17}},
 {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}},
 {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
 {'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
 {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,16,30,1,1,30}},
 {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
 {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
 {':',{0,4,4,0,4,4,0}}, {'-',{0,0,0,31,0,0,0}},
 {'.',{0,0,0,0,0,6,6}}, {'/',{1,2,2,4,8,8,16}},
 {'>',{16,8,4,2,4,8,16}}, {'?',{14,17,1,2,4,0,4}}
};
void ui_fb_text(UiFramebuffer *f,int x,int y,const char *s,int scale,UiColor color) {
    if (!s || scale<1 || scale>128) return;
    int64_t cursor=x;
    for (;*s;s++,cursor+=6*scale) {
        if (cursor>INT_MAX-5*scale) break;
        char c=*s;
        if (c>='a' && c<='z') c=(char)(c-'a'+'A');
        if (c==' ') continue;
        const uint8_t *rows=glyphs[sizeof(glyphs)/sizeof(glyphs[0])-1].rows;
        for (size_t i=0;i<sizeof(glyphs)/sizeof(glyphs[0]);i++)
            if (glyphs[i].c==c) { rows=glyphs[i].rows; break; }
        for (int gy=0;gy<7;gy++) for (int gx=0;gx<5;gx++)
            if (rows[gy]&(1u<<(4-gx))) {
                int64_t yy=(int64_t)y+gy*scale;
                if (yy<=INT_MAX) ui_fb_rect(f,(UiRect){(int)cursor+gx*scale,(int)yy,scale,scale},color);
            }
    }
}
