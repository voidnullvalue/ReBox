#include "image.h"
/* One scratch image, independent of how many installed modules exist. */
void draw_icon(UiFramebuffer *f,const unsigned char *pixels,int width,int height,int x,int y,int size,int focus){
    static unsigned char scaled[128*128*4];
    if(size<1||size>128||x+size<0||x>=f->width)return;if(focus<0)focus=0;if(focus>1000)focus=1000;
    if(pixels&&width>0&&height>0){image_scale(pixels,width,height,scaled,size,size);image_draw(f,scaled,size,size,x,y,105+150*focus/1000);}
    else{draw_round(f,x,y,size,size,size/8,UI_PANEL,UI_PANEL);int gap=size/12,cell=size/4;for(int i=0;i<4;i++)draw_round(f,x+size/4+(i%2)*(cell+gap),y+size/4+(i/2)*(cell+gap),cell,cell,cell/5,UI_MUTED,UI_MUTED);}
}
