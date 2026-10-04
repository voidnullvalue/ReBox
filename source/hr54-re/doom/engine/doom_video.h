#ifndef DOOM_VIDEO_H
#define DOOM_VIDEO_H
#include "framebuffer.h"
int doom_video_open(int width,int height,int depth);
int doom_video_present(const unsigned char *indices,const unsigned char *palette);
int doom_video_selftest(unsigned long sequence);
int doom_video_close(void);
#endif
