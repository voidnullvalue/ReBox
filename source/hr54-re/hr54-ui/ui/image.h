#ifndef HR54_UI_IMAGE_H
#define HR54_UI_IMAGE_H
#include "draw.h"
/* Area filtering when shrinking, bilinear when enlarging. Both filter
 * premultiplied color so transparent edges never acquire dark fringes. */
void image_scale(const unsigned char *,int,int,unsigned char *,int,int);
void image_draw(UiFramebuffer *,const unsigned char *,int,int,int,int,int);
#endif
