#ifndef HR54_UI_STATE_H
#define HR54_UI_STATE_H
#include "../api/services.h"
#include "../api/modules.h"
#include "../input/keys.h"
#include "../ui/artwork.h"
typedef enum { SCREEN_STARTUP,SCREEN_HOME,SCREEN_BROWSER,SCREEN_DETAILS,SCREEN_KEYBOARD,SCREEN_PLAYER,SCREEN_SETTINGS,SCREEN_PAIR,SCREEN_ERROR,SCREEN_NATIVE_APP,SCREEN_HIDDEN,SCREEN_MODULES,SCREEN_MODULE_DETAIL,SCREEN_MODULE_SETTINGS,SCREEN_ABOUT } Screen;
typedef enum { KEYBOARD_SEARCH,KEYBOARD_URL,KEYBOARD_FIELD } KeyboardMode;
#define NAV_DEPTH 12
/* History holds opaque cursors and selection only, never copies of media pages. */
typedef struct {char parent[256],title[256],query[65];int offset,selection;} BrowseNode;
typedef struct {
    Screen screen,previous,return_screen,error_return,keyboard_return;
    ModuleList modules;int home_modules[REBOX_MAX_MODULES],home_count,modules_loaded;
    char active_module[64],operation_module[64],return_module[64],managed_module[64],active_native_module[64],opening_module[64];
    BrowseNode navigation[NAV_DEPTH];int depth;MediaList media;ModuleSettings module_settings;
    int home,settings_focus,module_focus,keyboard_focus,keyboard_upper,ready,frontend_prepared,still,start_hidden;
    int loading,dirty,visible,native_app_running,native_app_starting,native_app_returning,native_release_surface,native_release_input,input_active,surface_open,status_failures;
    int module_open_stops,opening_prepare;
    int awaiting,player_focus,cancel_play,channel_changing,stop_after_play,edit_field,management_pair,refresh_modules;
    KeyboardMode keyboard_mode;char query[65],entry[1025],pair_code[40],poll_action[64],message[256],notice[160];
    uint64_t next_status,next_modules,next_action,prepare_due,animate_start,last_key,notice_until,art_due,player_until;
    int home_from,home_direction,home_motion;unsigned observed_play_generation,revealed_play_generation;
    PlaybackState playback;Artwork artwork;ApiClient api;
} App;
void app_init(App *,int);
void app_free(App *);
void app_key(void *,KeyEvent);
void app_tick(App *,uint64_t);
void app_response(void *,const ApiResponse *);
void app_load(App *);
BrowseNode *app_node(App *);
const MediaItem *app_selected(App *);
const ModuleDescriptor *app_module(const App *,const char *);
const ModuleDescriptor *app_home_module(const App *,int);
void app_rebuild_home(App *);
void app_art_key(char *,size_t,const char *,const char *);
const char *app_keyboard_text(const App *);
const char *app_keyboard_letters(const App *);
int app_keyboard_cells(const App *);
void app_error(App *,ApiError);
int app_animating(const App *,uint64_t);
void app_fixture(App *,const char *);
#endif
