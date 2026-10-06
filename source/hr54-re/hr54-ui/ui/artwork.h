#ifndef HR54_UI_ARTWORK_H
#define HR54_UI_ARTWORK_H
#include "draw.h"
#define ART_ENTRIES 8
#define ART_W UI_DETAIL_W
#define ART_H 160
typedef struct { char id[384];unsigned char *pixels;uint64_t used;int failed,width,height; } ArtEntry;
typedef struct ArtJob ArtJob;
typedef struct { ArtEntry entry[ART_ENTRIES];char pending[384];int pending_icon;ArtJob *job;pthread_t worker; } Artwork;
void artwork_free(Artwork *);
void artwork_reset(Artwork *);
int artwork_accept(Artwork *,const unsigned char *,size_t,int);
int artwork_queue(Artwork *,const unsigned char *,size_t,int,unsigned);
int artwork_poll(Artwork *,unsigned);
const ArtEntry *artwork_find(Artwork *,const char *);
void artwork_draw(UiFramebuffer *,const ArtEntry *,int,int);
#endif
