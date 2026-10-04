#include "image.h"
#include "icon_data.h"
/* One scaled image per service: bounded storage, reused while idle. */
static struct {int size;unsigned char pixels[ICON_SIZE*ICON_SIZE*4];} cache[5];
void draw_icon(UiFramebuffer *f,int source,int x,int y,int size,int focus){
    if(source<0||source>=5||size<1||size>ICON_SIZE||x+size<0||x>=f->width)return;
    if(focus<0)focus=0;if(focus>1000)focus=1000;
    if(cache[source].size!=size){image_scale(icon_rgba[source],ICON_SIZE,ICON_SIZE,cache[source].pixels,size,size);cache[source].size=size;}
    image_draw(f,cache[source].pixels,size,size,x,y,105+150*focus/1000);
}
