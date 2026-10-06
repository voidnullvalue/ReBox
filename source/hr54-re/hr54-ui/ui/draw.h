#ifndef HR54_UI_DRAW_H
#define HR54_UI_DRAW_H
#include "../platform.h"
#include "theme.h"
void draw_pixel(UiFramebuffer *,int,int,UiColor);
void draw_round(UiFramebuffer *,int,int,int,int,int,UiColor,UiColor);
void draw_line(UiFramebuffer *,int,int,int,int,int,UiColor);
void draw_circle(UiFramebuffer *,int,int,int,UiColor);
void draw_triangle(UiFramebuffer *,int,int,int,int,int,int,UiColor);
void draw_text(UiFramebuffer *,int,int,const char *,int,UiColor,int);
int text_width(const char *,int);
void draw_center(UiFramebuffer *,int,int,const char *,int,UiColor);
void draw_wrap(UiFramebuffer *,int,int,const char *,int,UiColor,int,int);
void draw_background(UiFramebuffer *,uint64_t);
void draw_icon(UiFramebuffer *,const unsigned char *,int,int,int,int,int,int);
#endif
