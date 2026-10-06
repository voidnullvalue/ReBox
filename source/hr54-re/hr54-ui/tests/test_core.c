#include "apps/state.h"
#include "input/dispatcher.h"
#include "ui/image.h"
#include <assert.h>
static void key(App *a,UiKey k){app_key(a,(KeyEvent){k,1,0,0});}
static void reply(App *a,ApiKind kind,const char *json){ApiResponse r={kind,API_OK,a->api.generation,200,(const unsigned char *)json,strlen(json)};app_response(a,&r);}
int main(void){
    /* Shrinking must average fine detail and preserve colored transparent edges. */
    unsigned char checker[]={0,0,0,255,255,255,255,255,255,255,255,255,0,0,0,255},pixel[4];image_scale(checker,2,2,pixel,1,1);assert(pixel[0]==128&&pixel[1]==128&&pixel[2]==128&&pixel[3]==255);
    unsigned char edge[]={255,0,0,255,0,0,255,0};image_scale(edge,2,1,pixel,1,1);assert(pixel[0]==255&&pixel[1]==0&&pixel[2]==0&&pixel[3]==128);
    unsigned char large[4*8*4];image_scale(edge,2,1,large,8,4);for(unsigned i=0;i<sizeof(large);i+=4)if(large[i+3])assert(large[i]==255&&large[i+2]==0);
    UiFramebuffer visual;assert(!ui_fb_init(&visual,UI_WIDTH,UI_HEIGHT));draw_background(&visual,0);unsigned char first=visual.pixels[(240*UI_WIDTH+360)*4];for(int i=0;i<UI_WIDTH*UI_HEIGHT;i++){unsigned char *p=visual.pixels+i*4;assert(p[0]==p[1]&&p[1]==p[2]&&p[0]<=24&&p[3]==255);}draw_background(&visual,32000);assert(visual.pixels[(240*UI_WIDTH+360)*4]!=first);ui_fb_clear(&visual,COLOR(0,0,0,0));draw_round(&visual,20,20,32,32,8,COLOR(255,255,255,255),COLOR(255,255,255,255));int partial=0;for(int i=0;i<UI_WIDTH*UI_HEIGHT;i++)partial+=visual.pixels[i*4+3]>0&&visual.pixels[i*4+3]<255;assert(partial);ui_fb_free(&visual);
    Json j;
    const char *bad[]={"", "{", "[]x", "{\"x\":01}", "{\"x\":.1}", "{\"x\":tru}", "{\"x\":1,}", "[1,]", "{\"x\":\"\\q\"}", "{\"x\":\"\n\"}"};for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)assert(json_open(&j,bad[i],strlen(bad[i]))<0);
    const char *good="{\"a\":[1,{\"title\":\"caf\\u00e9 \\ud83d\\udcfa\"}]}";assert(!json_open(&j,good,strlen(good)));char title[64];int a=json_field(&j,0,"a");assert(json_number(&j,json_nth(&j,a,0))==1);assert(!json_string(&j,json_field(&j,json_nth(&j,a,1),"title"),title,sizeof(title)));assert(!strcmp(title,"café 📺"));json_close(&j);
    assert(input_key(0x1e402)==KEY_STOP&&input_key(0xe001)==KEY_SELECT&&input_key(0x1e00b)==KEY_GUIDE);
    /* Captured RF PLAY/PAUSE is e401 for both presses, not e400 on resume. */
    App *toggle=calloc(1,sizeof(*toggle));assert(toggle);app_init(toggle,1);toggle->screen=SCREEN_HIDDEN;toggle->playback.playing=1;toggle->playback.can_pause=1;toggle->playback.can_resume=1;
    app_key(toggle,(KeyEvent){input_key(0x1e401),1,0,0x1e401});assert(strstr(toggle->api.r[API_CONTROL].request,"POST /api/playback/pause "));api_cancel(&toggle->api,API_CONTROL);
    toggle->playback.paused=1;app_key(toggle,(KeyEvent){input_key(0x1e401),1,1,0x1e401});assert(!api_busy(&toggle->api,API_CONTROL));
    app_key(toggle,(KeyEvent){input_key(0x1e401),1,0,0x1e401});assert(strstr(toggle->api.r[API_CONTROL].request,"POST /api/playback/resume "));api_cancel(&toggle->api,API_CONTROL);
    key(toggle,KEY_PLAY);assert(strstr(toggle->api.r[API_CONTROL].request,"POST /api/playback/resume "));api_cancel(&toggle->api,API_CONTROL);
    toggle->playback.can_resume=0;key(toggle,KEY_PAUSE);assert(!api_busy(&toggle->api,API_CONTROL));app_free(toggle);free(toggle);
    App *app=calloc(1,sizeof *app);assert(app);app_init(app,1);
    /* Home handles all bounded registry sizes, including no enabled entries. */
    for(int count=0;count<=REBOX_MAX_MODULES;count++){
        app->modules.count=count;app->home_count=0;app->home=0;app->screen=SCREEN_HOME;
        for(int i=0;i<count;i++){ModuleDescriptor *m=&app->modules.items[i];memset(m,0,sizeof *m);snprintf(m->id,sizeof m->id,"unknown-%d",i);snprintf(m->name,sizeof m->name,"Module %d",i);m->installed=m->enabled=m->healthy=m->home=m->browse=m->search=1;}
        app_rebuild_home(app);assert(app->home_count==count);key(app,KEY_LEFT);assert(app->home==(count?count-1:0));key(app,KEY_RIGHT);assert(app->home==0);
    }
    app->modules.items[0].enabled=0;app_rebuild_home(app);assert(app->home_count==31);assert(strcmp(app_home_module(app,0)->id,"unknown-0"));
    app->home=0;key(app,KEY_SELECT);assert(app->screen==SCREEN_BROWSER);assert(strstr(app->api.r[API_BROWSE].request,"/api/modules/unknown-1/browse?"));
    api_cancel(&app->api,API_BROWSE);
    reply(app,API_MEDIA,"{\"items\":[{\"id\":\"a/b:c\",\"title\":\"Folder\",\"kind\":\"folder\"}],\"total\":1,\"offset\":0,\"hasMore\":false}");
    key(app,KEY_SELECT);assert(app->depth==1);assert(strstr(app->api.r[API_BROWSE].request,"parent=a%2Fb%3Ac&"));
    api_cancel(&app->api,API_BROWSE);key(app,KEY_LEFT);assert(app->depth==0);api_cancel(&app->api,API_BROWSE);
    app->loading=0;key(app,KEY_INFO);assert(app->screen==SCREEN_KEYBOARD);ui_copy(app->query,sizeof app->query,"foo");app->keyboard_focus=44;key(app,KEY_SELECT);assert(strstr(app->api.r[API_BROWSE].request,"/search?parent=&q=foo&offset=0"));
    unsigned stale=app->api.generation;key(app,KEY_GUIDE);ApiResponse delayed={API_MEDIA,API_OK,stale,200,(const unsigned char *)"{\"items\":[]}",12};app_response(app,&delayed);assert(app->screen==SCREEN_HOME);
    /* Search-first behavior depends only on declared capabilities. */
    app->modules.items[1].browse=0;key(app,KEY_SELECT);assert(app->screen==SCREEN_KEYBOARD);key(app,KEY_GUIDE);app->modules.items[1].browse=1;
    /* URL entry has a separate bounded buffer and all required punctuation. */
    app->screen=SCREEN_KEYBOARD;app->keyboard_mode=KEYBOARD_URL;const char *symbols=":/.-_?&=%#~+@";for(const char *p=symbols;*p;p++){app->keyboard_focus=(int)(strchr(app_keyboard_letters(app),*p)-app_keyboard_letters(app));key(app,KEY_SELECT);}assert(!strcmp(app->entry,symbols));
    app->keyboard_focus=0;for(int i=0;i<1100;i++)key(app,KEY_SELECT);assert(strlen(app->entry)==1024);assert(strlen(app->query)==3);app->keyboard_focus=53;key(app,KEY_SELECT);assert(!*app->entry);app->keyboard_focus=50;key(app,KEY_SELECT);app->keyboard_focus=0;key(app,KEY_SELECT);assert(!strcmp(app->entry,"A"));
    /* Physical LEFT backs out once; held LEFT cannot pop a history stack. */
    app_fixture(app,"browser");app_key(app,(KeyEvent){KEY_LEFT,1,1,0});assert(app->screen==SCREEN_BROWSER);key(app,KEY_SELECT);assert(app->screen==SCREEN_DETAILS);key(app,KEY_LEFT);assert(app->screen==SCREEN_BROWSER);key(app,KEY_LEFT);assert(app->screen==SCREEN_HOME);
    app->screen=SCREEN_HOME;app->playback=(PlaybackState){0};app->dirty=0;reply(app,API_STATE,"{\"playing\":false,\"source\":\"\"}");assert(!app->dirty);
    reply(app,API_STATE,"{\"playing\":true,\"source\":\"external-module\",\"generation\":5,\"transport\":{\"stop\":true}}");assert(app->screen==SCREEN_HIDDEN&&app->dirty);key(app,KEY_GUIDE);reply(app,API_STATE,"{\"playing\":true,\"source\":\"external-module\",\"generation\":5}");assert(app->screen==SCREEN_HOME);
    app->awaiting=API_PLAY;reply(app,API_PLAY,"{\"playing\":true,\"source\":\"external-module\",\"generation\":6}");assert(app->screen==SCREEN_HOME); /* GUIDE during preparation stays visible. */
    reply(app,API_STATE,"{\"playing\":false,\"instance\":\"new-core-instance\",\"generation\":0}");assert(!app->playback.playing&&!app->observed_play_generation);
    ApiResponse timeout={API_STATE,API_TIMEOUT,app->api.generation,0,NULL,0};app->notice[0]=0;app_response(app,&timeout);assert(!*app->notice&&app->status_failures==1);
    app_free(app);app_init(app,1);app->start_hidden=1;reply(app,API_READY,"{\"ready\":true,\"frontend\":\"native\",\"nativeModule\":\"\",\"mediaBusy\":true}");assert(app->screen==SCREEN_HIDDEN&&!app->frontend_prepared);
    reply(app,API_STATE,"{\"playing\":true,\"source\":\"some-new-module\",\"generation\":1}");reply(app,API_STATE,"{\"playing\":false,\"generation\":1}");assert(app->screen==SCREEN_HOME);
    app->return_screen=SCREEN_DETAILS;ui_copy(app->return_module,sizeof app->return_module,"some-new-module");app->screen=SCREEN_PLAYER;app->playback.playing=1;app->loading=1;app->player_until=99999;
    reply(app,API_STATE,"{\"playing\":false,\"generation\":1}");assert(app->screen==SCREEN_DETAILS&&!app->loading&&!app->player_until&&!strcmp(app->active_module,"some-new-module"));
    app_free(app);app_init(app,1);reply(app,API_READY,"{\"ready\":true,\"frontend\":\"native\",\"mediaBusy\":true,\"nativeModule\":\"unknown-native\"}");assert(app->native_app_running&&app->screen==SCREEN_NATIVE_APP);reply(app,API_NATIVE_STATUS,"{\"running\":false}");assert(app->screen==SCREEN_HOME&&!app->native_app_running);
    Artwork art={0};ui_copy(art.pending,sizeof art.pending,"bad");assert(artwork_accept(&art,(const unsigned char *)"garbage",7,1)<0);assert(artwork_find(&art,"bad")->failed);artwork_free(&art);
    assert(sizeof(App)<256*1024);printf("PASS runtime Home 0..32, opaque navigation, search-first, URL keyboard, stale replies, generic playback/native restoration; App=%zu bytes\n",sizeof(App));
    app_free(app);free(app);return 0;
}
