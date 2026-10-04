#include "screens.h"
void render_keyboard(UiFramebuffer *f,App *a){render_header(f,a->source==SOURCE_YOUTUBE?"Search YouTube":a->source==SOURCE_IPTV?"Search live TV":"Search Jellyfin","Find something worth watching");draw_round(f,UI_SAFE_X,UI_CONTENT_Y-5,UI_WIDTH-2*UI_SAFE_X,48,6,UI_PANEL,UI_PANEL);draw_text(f,UI_SAFE_X+14,UI_CONTENT_Y+8,a->query[0]?a->query:"Type with your remote",22,a->query[0]?UI_INK:UI_MUTED,610);
    const char *s=a->keyboard_upper?"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.'?":"abcdefghijklmnopqrstuvwxyz0123456789-.'?";
    int width=(UI_WIDTH-2*UI_SAFE_X)/10;int start=UI_CONTENT_Y+70;
    for(int i=0;i<40;i++){int x=UI_SAFE_X+(i%10)*width,y=start+(i/10)*48;int focus=i==a->keyboard_focus;UiColor face=focus?UI_INK:UI_PANEL;draw_round(f,x+2,y,width-5,41,7,face,face);char letter[2]={s[i],0};draw_center(f,x+width/2,y+9,letter,22,focus?COLOR(18,18,22,255):UI_MUTED);}
    const char *labels[]={"Aa","Space","Delete","Clear","Search","Cancel"};const int col[]={0,1,2,3,4,8},span[]={1,1,1,1,4,2};for(int i=0;i<6;i++){int focus=i<4?a->keyboard_focus==40+i:i==4?a->keyboard_focus>=44&&a->keyboard_focus<=47:a->keyboard_focus>=48;render_button(f,UI_SAFE_X+col[i]*width+2,start+4*48,span[i]*width-5,labels[i],focus);}
    render_footer(f,"Arrows  Move     SELECT  Type","BACK  Delete / Return");
}
