#include "screens.h"
#include "animation.h"
static const char *titles[]={"Jellyfin","IPTV","Frigate","YouTube","Doom"};
static const char *descriptions[]={"Movies, shows and music","Live television","Your cameras, at a glance","Find something worth watching","Doom shareware"};
void render_home(UiFramebuffer *f,App *a,uint64_t now){
    draw_text(f,UI_SAFE_X,UI_HEADER_Y,"Media center",UI_FONT_BODY,UI_MUTED,300);
    draw_text(f,UI_WIDTH-UI_SAFE_X-75,UI_HEADER_Y,"Settings",UI_FONT_SMALL,UI_MUTED,75);
    int progress=ui_transition_progress(a->animate_start,now),motion=a->home_motion*(1000-progress)/1000;
    int order[]={-3,3,-2,2,-1,1,0};
    for(int i=0;i<7;i++){int delta=order[i]-motion/1000;if(progress==1000&&abs(delta)>2)continue;
        int source=((a->home+delta)%5+5)%5,position=delta*1000+motion;
        int focus=1000-abs(position);if(focus<0)focus=0;
        int size=52+(UI_TILE-52)*focus/1000,x=UI_WIDTH/2+position*UI_TILE_PITCH/1000-size/2,y=UI_TILE_Y+(UI_TILE-size)/2;
        draw_icon(f,source,x,y,size,focus);
        if(!focus&&abs(position)<=2100)draw_center(f,x+size/2,UI_TILE_Y+UI_TILE+18,titles[source],UI_FONT_SMALL,COLOR(116,116,124,190));
    }
    draw_center(f,UI_WIDTH/2,UI_HOME_TITLE_Y,titles[a->home],UI_FONT_HERO,UI_INK);
    draw_center(f,UI_WIDTH/2,UI_HOME_DESCRIPTION_Y,descriptions[a->home],UI_FONT_BODY,UI_MUTED);
    if(a->playback.playing){draw_text(f,UI_SAFE_X,UI_HOME_HINT_Y,"Now playing",UI_FONT_SMALL,UI_ACCENT,110);draw_text(f,UI_SAFE_X+104,UI_HOME_HINT_Y,a->playback.title,UI_FONT_BODY,UI_MUTED,UI_WIDTH-2*UI_SAFE_X-104);}
    render_footer(f,"←  →   Browse       SELECT  Open","↓  Settings     BACK  TV");
}
