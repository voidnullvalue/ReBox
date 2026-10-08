#ifndef HR54_UI_MODELS_H
#define HR54_UI_MODELS_H
#include "client.h"
#define UI_PAGE_SIZE 60
typedef struct {char id[256],title[256],subtitle[128],overview[512],type[32],artwork[256];int folder,playable,year;long duration,resume;} MediaItem;
typedef struct {MediaItem item[UI_PAGE_SIZE];int count,total,offset,has_more;} MediaList;
typedef struct {int playing,paused,live,can_pause,can_resume,can_seek,can_stop,can_channel_up,can_channel_down;long elapsed,duration;unsigned generation;char title[256],source[64],instance[33];} PlaybackState;
#endif
