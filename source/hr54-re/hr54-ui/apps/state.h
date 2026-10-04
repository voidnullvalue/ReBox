#ifndef HR54_UI_STATE_H
#define HR54_UI_STATE_H
#include "../api/services.h"
#include "../input/keys.h"
#include "../ui/artwork.h"
typedef enum { SCREEN_STARTUP,SCREEN_HOME,SCREEN_BROWSER,SCREEN_DETAILS,SCREEN_KEYBOARD,SCREEN_PLAYER,SCREEN_SETTINGS,SCREEN_PAIR,SCREEN_ERROR,SCREEN_DOOM,SCREEN_HIDDEN } Screen;
typedef enum { BROWSE_ROOT,BROWSE_ITEMS,BROWSE_RESUME,BROWSE_CHANNELS,BROWSE_RESULTS } BrowseKind;
#define NAV_DEPTH 12
typedef struct { BrowseKind kind;char parent[256],title[256],query[65];int offset,selection,loaded;MediaList list; } BrowseNode;
typedef struct { BrowseNode node[NAV_DEPTH];int depth; } SourceState;
typedef struct {
    Screen screen,previous,return_screen,error_return;Source source,operation_source,return_source;SourceState sources[4];
    int home,settings_focus,keyboard_focus,keyboard_upper,ready,frontend_prepared,still,start_hidden,authenticated,bitrate,loading,dirty,visible,doom_running,doom_starting,doom_returning,input_active,surface_open,status_failures;
    int awaiting,restore_selection,player_focus,cancel_play,stop_after_play;char query[65],pair_code[40],message[160],notice[160];
    int quality_count,quality_rates[8];
    uint64_t next_status,prepare_due,animate_start,last_key,notice_until,art_due,player_until;int home_from,home_direction,home_motion;unsigned observed_play_generation,revealed_play_generation;PlaybackState playback;Artwork artwork;ApiClient api;
} App;
void app_init(App *,int);
void app_free(App *);
void app_key(void *,KeyEvent);
void app_tick(App *,uint64_t);
void app_response(void *,const ApiResponse *);
void app_load(App *);
BrowseNode *app_node(App *);
const MediaItem *app_selected(App *);
void app_error(App *,ApiError);
int app_animating(const App *,uint64_t);
void app_fixture(App *,const char *);
#endif
