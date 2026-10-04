#include "menu_model.h"
#include <string.h>
static const UiColor white={235,241,248,255}, muted={149,165,190,255};
static const UiColor panel={18,25,40,255}, accent={40,139,240,255};
static const char *labels[]={"Jellyfin","IPTV","YouTube","Recordings"};
void ui_menu_init(UiMenu *m) {
    memset(m,0,sizeof(*m)); m->visible=1; m->previous=-1; m->full_redraw=1;
}
int ui_menu_key(UiMenu *m, UiKey key) {
    switch (key) {
    case UI_KEY_MENU: m->visible=!m->visible; m->full_redraw=1; return 1;
    case UI_KEY_BACK: if (m->visible) { m->visible=0; m->full_redraw=1; return 1; } return 0;
    case UI_KEY_UP: if (m->visible) { m->selected=(m->selected+3)%4; return 1; } return 0;
    case UI_KEY_DOWN: if (m->visible) { m->selected=(m->selected+1)%4; return 1; } return 0;
    case UI_KEY_SELECT: if (m->visible && m->select_source) m->select_source(m->control,m->selected); return 0;
    case UI_KEY_PLAY_PAUSE: m->paused=!m->paused; if (m->set_rate) m->set_rate(m->control,m->paused?0:1000); return 0;
    case UI_KEY_STOP: if (m->stop) m->stop(m->control); return 0;
    }
    return 0;
}
int ui_menu_progress(UiMenu *m, unsigned p) {
    if (p>1000) p=1000;
    if (m->progress==p) return 0;
    m->progress=p; return m->visible;
}
static void row(UiFramebuffer *f, int i, int selected) {
    int y=132+i*62;
    ui_fb_rect(f,(UiRect){60,y,400,52},selected?accent:panel);
    ui_fb_text(f,84,y+16,labels[i],3,white);
    if (selected) ui_fb_text(f,424,y+16,">",3,white);
}
void ui_menu_render(UiMenu *m,UiFramebuffer *f) {
    if (m->full_redraw) {
        ui_fb_clear(f,(UiColor){0,0,0,0});
        if (m->visible) {
            ui_fb_rect(f,(UiRect){40,32,440,422},panel);
            ui_fb_text(f,60,54,"Media",5,white);
            ui_fb_text(f,60,104,"Choose a source",2,muted);
            /* Original software bitmap icon; demonstrates image blitting. */
            uint8_t icon[16*16*4];
            for (int y=0;y<16;y++) for (int x=0;x<16;x++) {
                int at=(y*16+x)*4;
                icon[at]=40; icon[at+1]=139; icon[at+2]=240;
                icon[at+3]=(uint8_t)(x>=3 && x<=11 && y>=3 && y<=12?255:0);
            }
            ui_fb_blit(f,440,58,icon,16,16,64);
            for (int i=0;i<4;i++) row(f,i,i==m->selected);
            ui_fb_text(f,60,396,"Now playing",2,muted);
        }
    } else if (m->visible && m->previous!=m->selected) {
        row(f,m->previous,0); row(f,m->selected,1);
    }
    if (m->visible && (m->full_redraw || m->progress!=m->painted_progress)) {
        ui_fb_rect(f,(UiRect){60,426,400,8},(UiColor){49,61,81,255});
        ui_fb_rect(f,(UiRect){60,426,(int)(400*m->progress/1000),8},accent);
    }
    m->full_redraw=0; m->previous=m->selected; m->painted_progress=m->progress;
}
