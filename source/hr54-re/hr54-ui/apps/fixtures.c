/* Host-only data fixtures. None of these IDs are runtime dispatch choices. */
#include "state.h"
#include "preview_art.h"
void app_fixture(App *a,const char *name){memset(&a->media,0,sizeof a->media);memset(a->navigation,0,sizeof a->navigation);memset(&a->modules,0,sizeof a->modules);artwork_reset(&a->artwork);a->depth=a->home=a->home_count=0;a->ready=a->frontend_prepared=a->modules_loaded=1;a->loading=0;a->screen=SCREEN_HOME;int count=5;
    if(!strcmp(name,"home-zero"))count=0;else if(!strcmp(name,"home-one"))count=1;else if(!strcmp(name,"home-ten"))count=10;
    for(int i=0;i<count;i++){ModuleDescriptor *m=&a->modules.items[i];snprintf(m->id,sizeof m->id,"example-%d",i);snprintf(m->name,sizeof m->name,"Module %d",i+1);ui_copy(m->description,sizeof m->description,"Independent receiver module");ui_copy(m->version,sizeof m->version,"1.0");m->installed=m->enabled=m->healthy=m->compatible=m->home=m->browse=m->search=m->playback=1;}
    a->modules.count=count;app_rebuild_home(a);
    if(!strcmp(name,"home-disabled")&&count){a->modules.items[0].enabled=0;app_rebuild_home(a);return;}
    if(!strcmp(name,"home-unhealthy")&&count){a->modules.items[0].healthy=0;return;}
    if(!strcmp(name,"home-native")&&count){a->modules.items[0].native_app=1;a->modules.items[0].release_input=a->modules.items[0].release_surface=1;return;}
    if(!strcmp(name,"home-search")&&count){a->modules.items[0].browse=0;return;}
    if(!strncmp(name,"home",4))return;
    if(!strcmp(name,"loading")){a->screen=SCREEN_STARTUP;return;}
    if(!strcmp(name,"error")){a->screen=SCREEN_ERROR;ui_copy(a->message,sizeof a->message,"The module is unavailable. Try again or open its settings.");return;}
    if(!strcmp(name,"settings")){a->screen=SCREEN_SETTINGS;return;}
    if(!strcmp(name,"modules")){a->screen=SCREEN_MODULES;return;}
    if(!strcmp(name,"pair")){a->screen=SCREEN_PAIR;a->management_pair=1;ui_copy(a->pair_code,sizeof a->pair_code,"849ab231");ui_copy(a->message,sizeof a->message,"Enter this code in Android module management.");return;}
    ui_copy(a->active_module,sizeof a->active_module,a->modules.items[0].id);
    if(!strcmp(name,"keyboard")||!strcmp(name,"url-keyboard")){a->screen=SCREEN_KEYBOARD;a->keyboard_mode=!strcmp(name,"url-keyboard")?KEYBOARD_URL:KEYBOARD_SEARCH;ui_copy(a->query,sizeof a->query,"architecture");ui_copy(a->entry,sizeof a->entry,"https://example.com/my-module.rbox");a->keyboard_focus=12;return;}
    if(!strcmp(name,"player-osd")){a->screen=SCREEN_PLAYER;a->playback=(PlaybackState){.playing=1,.paused=1,.can_pause=1,.can_resume=1,.can_seek=1,.can_stop=1,.elapsed=1638,.duration=6540};ui_copy(a->playback.title,sizeof a->playback.title,"The Quiet Horizon");return;}
    a->screen=!strcmp(name,"details")?SCREEN_DETAILS:SCREEN_BROWSER;ui_copy(a->navigation[0].title,sizeof a->navigation[0].title,"Movies");a->media.count=6;a->media.total=42;a->media.has_more=1;
    const char *titles[]={"The Quiet Horizon","North by Morning","Across the Blue","An Ordinary Adventure","A Room in the City","The Last Observatory"};
    for(int i=0;i<6;i++){MediaItem *x=&a->media.item[i];snprintf(x->id,sizeof x->id,"preview%d",i);ui_copy(x->artwork,sizeof x->artwork,x->id);ui_copy(x->title,sizeof x->title,titles[i]);x->playable=1;x->year=2024;x->duration=6540;ui_copy(x->type,sizeof x->type,"item");ui_copy(x->overview,sizeof x->overview,"A journey to the edge of the familiar, where a quiet discovery changes everything.");}
    ArtEntry *e=&a->artwork.entry[0];app_art_key(e->id,sizeof e->id,a->active_module,"preview0");e->width=ART_W;e->height=ART_H;e->pixels=calloc(ART_W*ART_H,4);if(e->pixels)memcpy(e->pixels,preview_art,ART_W*ART_H*4);
}
