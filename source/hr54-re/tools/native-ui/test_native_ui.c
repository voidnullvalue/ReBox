#include "framebuffer.h"
#include "menu_model.h"
#include "native_egl.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
static int events[256], nevents, fail_step, swap_fail, swaps, finish_count, clears;
static UiRect uploaded;
static UiRect viewport_rect;
static uint8_t first_pixel[4];
static void event(int e) { assert(nevents<256); events[nevents++]=e; }
static Hr54EglHandle display(int id) { assert(id==0); event(1); return (void *)1; }
static int initialize(Hr54EglHandle d,int *a,int *b) { assert(d==(void *)1); *a=1; *b=1; event(2); return fail_step!=2; }
static int configs(Hr54EglHandle d,Hr54EglHandle *c,int max,int *n) { (void)d; assert(max==1); *c=(void *)2; *n=1; event(3); return 1; }
static Hr54EglHandle context(Hr54EglHandle d,Hr54EglHandle c,Hr54EglHandle share,const int *attr) { (void)d; assert(c==(void *)2 && !share && !attr); event(4); return (void *)3; }
static Hr54EglHandle window(Hr54EglHandle d,Hr54EglHandle c,int x,int y,int z,const int *attr) { (void)d; (void)c; assert(!x && !y && !z && !attr); event(5); return fail_step==5?0:(void *)4; }
static int current(Hr54EglHandle d,Hr54EglHandle draw,Hr54EglHandle read,Hr54EglHandle c) { (void)d; assert(draw==read); assert((draw && c) || (!draw && !c)); event(c?6:12); return 1; }
static int query(Hr54EglHandle d,Hr54EglHandle s,int what,int *v) { (void)d; (void)s; assert(what==0x3057 || what==0x3056); *v=what==0x3057?1920:1080; return fail_step!=8; }
static int depth(int z) { assert(z==10); event(7); return 1; }
static int swap(Hr54EglHandle d,Hr54EglHandle s) { (void)d; (void)s; event(10); swaps++; return !swap_fail; }
static int destroy_surface(Hr54EglHandle d,Hr54EglHandle s) { (void)d; assert(s==(void *)4); event(13); return 1; }
static int destroy_context(Hr54EglHandle d,Hr54EglHandle c) { (void)d; assert(c==(void *)3); event(14); return 1; }
static int terminate(Hr54EglHandle d) { assert(d==(void *)1); event(15); return 1; }
static void gen(int n,unsigned *t) { assert(n==1); *t=9; }
static void del(int n,const unsigned *t) { assert(n==1 && *t==9); event(11); }
static void bind(unsigned target,unsigned t) { assert(target==0xde1 && t==9); }
static void image(unsigned t,int l,int f,int w,int h,int border,unsigned fmt,unsigned type,const void *p) {
    assert(t==0xde1 && !l && f==0x1908 && w==720 && h==480 && !border && fmt==0x1908 && type==0x1401 && !p);
}
static void upload(unsigned t,int l,int x,int y,int w,int h,unsigned fmt,unsigned type,const void *p) {
    assert(t==0xde1 && !l && fmt==0x1908 && type==0x1401 && p);
    uploaded=(UiRect){x,y,w,h}; memcpy(first_pixel,p,4); event(8);
    /* Each row must be packed, not separated by framebuffer stride. */
    if (w==2 && h==2) assert(((const uint8_t *)p)[8]==77);
}
static void pixel_store(unsigned x,int v) { assert(x==0xcf5 && v==1); }
static void enable(unsigned x) { assert(x==0xde1 || x==0xbe2); }
static void client_state(unsigned x) { assert(x==0x8074 || x==0x8078); }
static void blend_func(unsigned s,unsigned d) { assert(s==0x302 && d==0x303); }
static void matrix(unsigned x) { assert(x==0x1700); }
static void identity(void) {}
static void scale(int x,int y,int z) { assert(x==131072 && y==131072 && z==65536); }
static void translate(int x,int y,int z) { assert(x==-32768 && y==-32768 && z==-131072); }
static void viewport(int x,int y,int w,int h) { viewport_rect=(UiRect){x,y,w,h}; }
static void pointer(int n,unsigned type,int stride,const void *p) { assert(n==2 && type==0x140c && !stride && p); }
static void draw(unsigned mode,int first,int count) { assert(mode==5 && !first && count==4); event(9); }
static void finish(void) { finish_count++; }
static void clear(unsigned mask) { assert(mask==0x4000); assert(viewport_rect.x==0 && viewport_rect.y==0 && viewport_rect.w==1920 && viewport_rect.h==1080); clears++; event(16); }
static unsigned error(void) { return 0; }
static const Hr54EglApi fake={display,initialize,configs,context,window,current,query,depth,swap,
    destroy_surface,destroy_context,terminate,gen,del,bind,image,upload,pixel_store,enable,
    client_state,blend_func,matrix,identity,scale,translate,viewport,pointer,pointer,draw,finish,error,0,clear};
static int rates[2], nrates, source=-1, stopped;
static void set_rate(void *ctx,int rate) { assert(ctx==(void *)8); rates[nrates++]=rate; }
static void select_source(void *ctx,int i) { assert(ctx==(void *)8); source=i; }
static void stop(void *ctx) { assert(ctx==(void *)8); stopped++; }
static void pixels(void) {
    UiFramebuffer f; assert(ui_fb_init(&f,4,3)==0);
    ui_fb_clean(&f);
    ui_fb_rect(&f,(UiRect){-1,-1,3,3},(UiColor){255,0,0,128});
    assert(f.dirty.x==0 && f.dirty.y==0 && f.dirty.w==2 && f.dirty.h==2);
    assert(f.pixels[0]==255 && f.pixels[1]==0 && f.pixels[2]==0 && f.pixels[3]==128);
    ui_fb_rect(&f,(UiRect){0,0,1,1},(UiColor){0,0,255,128});
    assert(f.pixels[0]==85 && f.pixels[2]==170 && f.pixels[3]==192);
    ui_fb_clean(&f);
    ui_fb_rect(&f,(UiRect){INT_MAX,0,INT_MAX,1},(UiColor){1,2,3,255});
    assert(!f.dirty.w);
    uint8_t img[]={1,2,3,255,4,5,6,255};
    ui_fb_blit(&f,-1,2,img,2,1,8);
    assert(!memcmp(f.pixels+2*f.stride,(uint8_t[]){4,5,6,255},4));
    ui_fb_clean(&f);
    ui_fb_blit(&f,0,0,img,2,1,4); /* reject undersized source stride */
    assert(!f.dirty.w);
    ui_fb_free(&f);
    assert(ui_fb_init(&f,0,3)==-1);
}
static void menus(void) {
    UiFramebuffer f,reference;
    assert(!ui_fb_init(&f,720,480) && !ui_fb_init(&reference,720,480));
    UiMenu m; ui_menu_init(&m); m.control=(void *)8;
    m.set_rate=set_rate; m.select_source=select_source; m.stop=stop;
    ui_menu_render(&m,&f); ui_fb_clean(&f);
    assert(ui_menu_key(&m,UI_KEY_DOWN)); ui_menu_render(&m,&f);
    assert(f.dirty.y==132 && f.dirty.h==114 && f.dirty.w==400);
    UiMenu full=m; full.full_redraw=1; ui_menu_render(&full,&reference);
    assert(!memcmp(f.pixels,reference.pixels,f.stride*(size_t)f.height));
    ui_fb_clean(&f); ui_menu_render(&m,&f); assert(!f.dirty.w);
    for (int i=0;i<50;i++) {
        ui_menu_key(&m,i%3?UI_KEY_DOWN:UI_KEY_UP);
        ui_menu_progress(&m,(unsigned)(i*27)); ui_menu_render(&m,&f);
        full=m; full.full_redraw=1; ui_menu_render(&full,&reference);
        assert(!memcmp(f.pixels,reference.pixels,f.stride*(size_t)f.height));
        ui_fb_clean(&f);
    }
    ui_menu_key(&m,UI_KEY_SELECT); assert(source==m.selected);
    ui_menu_key(&m,UI_KEY_PLAY_PAUSE); ui_menu_key(&m,UI_KEY_PLAY_PAUSE);
    assert(rates[0]==0 && rates[1]==1000);
    ui_menu_key(&m,UI_KEY_STOP); assert(stopped==1);
    ui_menu_key(&m,UI_KEY_BACK); ui_menu_render(&m,&f);
    for (size_t i=3;i<f.stride*(size_t)f.height;i+=4) assert(f.pixels[i]==0);
    ui_menu_key(&m,UI_KEY_MENU); ui_menu_render(&m,&f); assert(m.visible);
    ui_fb_free(&f); ui_fb_free(&reference);
}
static void presentation(void) {
    Hr54EglSurface s; UiFramebuffer f; assert(!ui_fb_init(&f,720,480));
    assert(!hr54_egl_open(&s,&fake,720,480,10));
    for (int i=0;i<7;i++) assert(events[i]==i+1);
    assert(hr54_egl_submit(&s,0,(UiRect){0,0,1,1})==-1);
    UiFramebuffer invalid=f; invalid.stride=SIZE_MAX;
    assert(hr54_egl_submit(&s,&invalid,(UiRect){0,0,1,1})==-1);
    UiSurface surface={&s,hr54_egl_submit};
    assert(!ui_present(&surface,&f) && !f.dirty.w);
    assert(uploaded.w==720 && uploaded.h==480 && swaps==1);
    assert(viewport_rect.x==0 && viewport_rect.y==0 && viewport_rect.w==1920 && viewport_rect.h==1080);
    assert(clears==0);s.replace_frame=1;
    s.aspect_width=4; s.aspect_height=3;
    ui_fb_rect(&f,(UiRect){5,7,2,2},(UiColor){77,8,9,255});
    swap_fail=1;
    assert(ui_present(&surface,&f)==-1 && f.dirty.w==2);
    assert(uploaded.x==5 && uploaded.y==7 && uploaded.w==2 && uploaded.h==2);
    swap_fail=0; assert(!ui_present(&surface,&f) && !f.dirty.w);
    assert(viewport_rect.x==240 && viewport_rect.y==0 && viewport_rect.w==1440 && viewport_rect.h==1080);
    assert(clears==2);assert(events[nevents-3]==16 && events[nevents-2]==9 && events[nevents-1]==10);
    assert(finish_count==0); /* BIST publishes without an extra finish/flush each frame. */
    int before=swaps; assert(!ui_present(&surface,&f) && before==swaps);
    assert(!hr54_egl_close(&s));
    assert(!first_pixel[0] && !first_pixel[1] && !first_pixel[2] && !first_pixel[3]);
    assert(finish_count>0 && events[nevents-3]==13 && events[nevents-2]==14 && events[nevents-1]==15);
    assert(!hr54_egl_close(&s));
    nevents=0; fail_step=5;
    assert(hr54_egl_open(&s,&fake,720,480,10)==-1);
    assert(events[nevents-2]==14 && events[nevents-1]==15);
    nevents=0; fail_step=8;
    assert(hr54_egl_open(&s,&fake,720,480,10)==-1);
    assert(events[nevents-3]==13 && events[nevents-2]==14 && events[nevents-1]==15);
    nevents=0; fail_step=2;
    assert(hr54_egl_open(&s,&fake,720,480,10)==-1 && nevents==2);
    assert(hr54_egl_open(&s,&fake,65536,480,10)==-1 && nevents==2);
    assert(hr54_egl_open(0,&fake,720,480,10)==-1 && nevents==2);
    ui_fb_free(&f);
}
int main(void) { pixels(); menus(); presentation(); puts("native-ui: raster, input model, incremental repaint, and fake EGL lifecycle passed"); }
