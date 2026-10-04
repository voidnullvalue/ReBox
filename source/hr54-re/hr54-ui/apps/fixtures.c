/* Host-only fixtures. Never linked into the receiver executable. */
#include "state.h"
#include "preview_art.h"
void app_fixture(App *a,const char *name){a->ready=1;a->frontend_prepared=1;a->authenticated=1;a->loading=0;a->screen=SCREEN_HOME;a->home=0;
    if(!strcmp(name,"home-doom")){a->home=4;return;}if(!strcmp(name,"home-jellyfin"))return;
    if(!strcmp(name,"loading")){a->screen=SCREEN_STARTUP;return;}
    if(!strcmp(name,"error")){a->screen=SCREEN_ERROR;ui_copy(a->message,sizeof(a->message),"Your library is unavailable right now. Check your connection and try again.");return;}
    if(!strcmp(name,"settings")){a->screen=SCREEN_SETTINGS;return;}
    if(!strcmp(name,"quick-connect")){a->screen=SCREEN_PAIR;ui_copy(a->pair_code,sizeof(a->pair_code),"849 231");return;}
    if(!strcmp(name,"keyboard")){a->screen=SCREEN_KEYBOARD;a->source=SOURCE_YOUTUBE;ui_copy(a->query,sizeof(a->query),"architecture");a->keyboard_focus=12;return;}
    if(!strcmp(name,"player-osd")){a->screen=SCREEN_PLAYER;a->playback=(PlaybackState){.playing=1,.paused=1,.can_pause=1,.can_resume=1,.can_seek=1,.can_stop=1,.elapsed=1638,.duration=6540};ui_copy(a->playback.title,sizeof(a->playback.title),"The Quiet Horizon");return;}
    const char *titles[6];a->screen=SCREEN_BROWSER;
    if(!strcmp(name,"iptv-browser")){a->source=SOURCE_IPTV;const char *t[]={"World News","Discovery","Cinema Classics","Music Live","The Sports Network","Arts & Culture"};memcpy(titles,t,sizeof(t));}
    else if(!strcmp(name,"frigate-cameras")){a->source=SOURCE_FRIGATE;const char *t[]={"Front entrance","Driveway","Garden","Back porch","Side gate","Garage"};memcpy(titles,t,sizeof(t));}
    else if(!strcmp(name,"youtube-results")){a->source=SOURCE_YOUTUBE;const char *t[]={"The architecture of tomorrow","A walk through Tokyo","Inside a recording studio","Designing a better city","Building with light","The art of a quiet space"};memcpy(titles,t,sizeof(t));}
    else {a->source=SOURCE_JELLYFIN;const char *t[]={"The Quiet Horizon","North by Morning","Across the Blue","An Ordinary Adventure","A Room in the City","The Last Observatory"};memcpy(titles,t,sizeof(t));}
    BrowseNode *n=app_node(a);n->loaded=1;n->kind=BROWSE_ITEMS;n->list.count=6;n->list.total=42;n->list.has_more=1;ui_copy(n->title,sizeof(n->title),a->source==SOURCE_JELLYFIN?"Movies":a->source==SOURCE_IPTV?"Entertainment":a->source==SOURCE_FRIGATE?"Your cameras":"Search results");
    for(int i=0;i<6;i++){MediaItem *x=&n->list.item[i];snprintf(x->id,sizeof(x->id),"preview%d",i);ui_copy(x->title,sizeof(x->title),titles[i]);ui_copy(x->subtitle,sizeof(x->subtitle),a->source==SOURCE_YOUTUBE?"Studio Archive":a->source==SOURCE_FRIGATE?"Live camera":"");x->playable=1;x->year=a->source==SOURCE_JELLYFIN?2024:0;x->duration=6540;ui_copy(x->type,sizeof(x->type),"Movie");ui_copy(x->overview,sizeof(x->overview),"A journey to the edge of the familiar, where a quiet discovery changes everything. An intimate story about finding your way home.");}
    if(a->source==SOURCE_JELLYFIN){ArtEntry *e=&a->artwork.entry[0];ui_copy(e->id,sizeof(e->id),"preview0");e->pixels=calloc(ART_W*ART_H,4);memcpy(e->pixels,preview_art,ART_W*ART_H*4);UiFramebuffer poster={.pixels=e->pixels,.width=ART_W,.height=ART_H,.stride=ART_W*4};draw_text(&poster,16,113,"The Quiet Horizon",18,COLOR(244,244,237,240),ART_W-32);if(!strcmp(name,"jellyfin-details"))a->screen=SCREEN_DETAILS;}
}
