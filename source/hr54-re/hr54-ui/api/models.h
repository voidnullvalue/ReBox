#ifndef HR54_UI_MODELS_H
#define HR54_UI_MODELS_H
#include "client.h"
#define UI_PAGE_SIZE 60
typedef struct { char id[256],title[256],subtitle[128],overview[512],type[32],artwork[256]; int folder,playable,year,virtual_entry; long duration,resume; } MediaItem;
typedef struct { MediaItem item[UI_PAGE_SIZE]; int count,total,offset,has_more; } MediaList;
/* Additive backend session identity: monotonically increasing generation per
 * committed playback session (including replacements) and the source the
 * backend reports as active. active_source is a Source value or -1 when no
 * active source identity is available; it is a hint for source-aware stop,
 * never a substitute for the UI's own operation tracking. */
typedef struct { int playing,paused,live,can_pause,can_resume,can_seek,can_stop; long elapsed,duration; unsigned generation; signed char active_source; char title[256],source[64]; } PlaybackState;
typedef struct { MediaList list; PlaybackState playback; int authenticated,pending,running,ready,prepared,busy,native,bitrate,quality_count,quality_rates[8]; char code[40]; } ApiModel;
int api_model(const ApiResponse *,ApiModel *);
#endif
