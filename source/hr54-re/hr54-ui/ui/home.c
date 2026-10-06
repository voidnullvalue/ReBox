#include "screens.h"
#include "animation.h"
void render_module_icon(UiFramebuffer *f,App *a,const char *id,int x,int y,int size,int focus){char key[384];app_art_key(key,sizeof key,id,NULL);const ArtEntry *e=artwork_find(&a->artwork,key);draw_icon(f,e?e->pixels:NULL,e?e->width:0,e?e->height:0,x,y,size,focus);}
void render_home(UiFramebuffer *f,App *a,uint64_t now){
    draw_text(f,UI_SAFE_X,UI_HEADER_Y,"ReBox",UI_FONT_BODY,UI_MUTED,300);draw_text(f,UI_WIDTH-UI_SAFE_X-75,UI_HEADER_Y,"Settings",UI_FONT_SMALL,UI_MUTED,75);
    if(!a->home_count){draw_center(f,UI_WIDTH/2,UI_HOME_TITLE_Y,a->modules_loaded?"No modules enabled":"Discovering modules",UI_FONT_TITLE,UI_INK);draw_center(f,UI_WIDTH/2,UI_HOME_DESCRIPTION_Y,"Open Settings → Modules",UI_FONT_BODY,UI_MUTED);render_footer(f,"↓  Settings","");return;}
    int progress=ui_transition_progress(a->animate_start,now),motion=a->home_motion*(1000-progress)/1000;
    /* Draw a viewport, not one tile for every installed module. Small catalogs
       draw each module once; larger ones wrap a bounded neighbor window. */
    int radius=a->home_count>5?3:a->home_count/2;
    for(int delta=-radius;delta<=radius;delta++){
        if(a->home_count<=5&&(delta<-a->home||delta>=a->home_count-a->home))continue;
        if(progress==1000&&abs(delta)>2)continue;int index=(a->home+delta+a->home_count)%a->home_count,position=delta*1000+motion;
        const ModuleDescriptor *m=app_home_module(a,index);int focus=1000-abs(position);if(focus<0)focus=0;
        int size=52+(UI_TILE-52)*focus/1000,x=UI_WIDTH/2+position*UI_TILE_PITCH/1000-size/2,y=UI_TILE_Y+(UI_TILE-size)/2;
        render_module_icon(f,a,m->id,x,y,size,focus);if(!focus&&abs(position)<=2100)draw_center(f,x+size/2,UI_TILE_Y+UI_TILE+18,m->name,UI_FONT_SMALL,COLOR(116,116,124,190));
    }
    const ModuleDescriptor *selected=app_home_module(a,a->home);draw_center(f,UI_WIDTH/2,UI_HOME_TITLE_Y,selected->name,UI_FONT_HERO,UI_INK);
    draw_center(f,UI_WIDTH/2,UI_HOME_DESCRIPTION_Y,selected->healthy?selected->description:"Module unavailable — open Settings → Modules",UI_FONT_BODY,UI_MUTED);
    if(a->playback.playing){draw_text(f,UI_SAFE_X,UI_HOME_HINT_Y,"Now playing",UI_FONT_SMALL,UI_ACCENT,110);draw_text(f,UI_SAFE_X+104,UI_HOME_HINT_Y,a->playback.title,UI_FONT_BODY,UI_MUTED,UI_WIDTH-2*UI_SAFE_X-104);}
    render_footer(f,"←  →   Browse       SELECT  Open","↓  Settings     BACK  TV");
}
