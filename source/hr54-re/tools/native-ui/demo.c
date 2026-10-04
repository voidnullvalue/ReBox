#include "menu_model.h"
#include <stdio.h>
int main(int argc,char **argv) {
    if (argc!=2) { fprintf(stderr,"usage: %s output.rgba\n",argv[0]); return 2; }
    UiFramebuffer f; UiMenu menu; ui_menu_init(&menu);
    if (ui_fb_init(&f,720,480)) return 1;
    ui_menu_progress(&menu,375); ui_menu_render(&menu,&f);
    FILE *out=fopen(argv[1],"wb");
    if (!out) { ui_fb_free(&f); return 1; }
    size_t bytes=f.stride*(size_t)f.height;
    int result=fwrite(f.pixels,1,bytes,out)!=bytes;
    if (fclose(out)) result=1;
    ui_fb_free(&f); return result;
}
