#include "apps/state.h"
#include "input/dispatcher.h"
#include "ui/image.h"
#include <assert.h>
static void key(App *a,UiKey k);
static void reply(App *a,ApiKind kind,const char *json){ApiResponse r={kind,API_OK,a->api.generation,200,(const unsigned char *)json,strlen(json)};app_response(a,&r);if(kind==API_MODULE_OPEN&&a->opening_prepare){api_cancel(&a->api,API_CONTROL);ApiResponse prepared={API_PREPARE,API_OK,a->api.generation,200,(const unsigned char *)"{\"prepared\":true}",17};app_response(a,&prepared);}}
static void key(App *a,UiKey k){app_key(a,(KeyEvent){k,1,0,0});if(a->api.r[API_OPERATION].kind==API_MODULE_OPEN&&api_busy(&a->api,API_OPERATION)){api_cancel(&a->api,API_OPERATION);reply(a,API_MODULE_OPEN,"{\"ready\":true,\"frontend\":\"native\",\"nativeModule\":\"\",\"mediaBusy\":false}");}}
int main(void){
    /* Shrinking must average fine detail and preserve colored transparent edges. */
    unsigned char checker[]={0,0,0,255,255,255,255,255,255,255,255,255,0,0,0,255},pixel[4];image_scale(checker,2,2,pixel,1,1);assert(pixel[0]==128&&pixel[1]==128&&pixel[2]==128&&pixel[3]==255);
    unsigned char edge[]={255,0,0,255,0,0,255,0};image_scale(edge,2,1,pixel,1,1);assert(pixel[0]==255&&pixel[1]==0&&pixel[2]==0&&pixel[3]==128);
    unsigned char large[4*8*4];image_scale(edge,2,1,large,8,4);for(unsigned i=0;i<sizeof(large);i+=4)if(large[i+3])assert(large[i]==255&&large[i+2]==0);
    UiFramebuffer visual;assert(!ui_fb_init(&visual,UI_WIDTH,UI_HEIGHT));draw_background(&visual,0);size_t frame_bytes=UI_WIDTH*UI_HEIGHT*4;unsigned char *first=malloc(frame_bytes);assert(first);memcpy(first,visual.pixels,frame_bytes);int black=0,brightest=0;for(int i=0;i<UI_WIDTH*UI_HEIGHT;i++){unsigned char *p=visual.pixels+i*4;assert(p[0]==p[1]&&p[1]==p[2]&&p[0]<=18&&p[3]==255);black+=p[0]==0;if(p[0]>brightest)brightest=p[0];}assert(black&&brightest>=9);draw_background(&visual,32000);assert(memcmp(visual.pixels,first,frame_bytes));free(first);ui_fb_clear(&visual,COLOR(0,0,0,0));draw_round(&visual,20,20,32,32,8,COLOR(255,255,255,255),COLOR(255,255,255,255));int partial=0;for(int i=0;i<UI_WIDTH*UI_HEIGHT;i++)partial+=visual.pixels[i*4+3]>0&&visual.pixels[i*4+3]<255;assert(partial);ui_fb_free(&visual);
    /* Opening uses fresh core ownership, waits for clean stop, then enters the saved target. */
    App *handoff=calloc(1,sizeof *handoff);assert(handoff);app_init(handoff,1);app_fixture(handoff,"home-five");
    handoff->home=0;const ModuleDescriptor *target=app_home_module(handoff,0);char target_id[64];ui_copy(target_id,sizeof target_id,target->id);
    app_key(handoff,(KeyEvent){KEY_SELECT,1,0,0});assert(*handoff->opening_module&&handoff->screen==SCREEN_HOME);assert(strstr(handoff->api.r[API_OPERATION].request,"GET /api/system/status "));
    api_cancel(&handoff->api,API_OPERATION);reply(handoff,API_MODULE_OPEN,"{\"ready\":true,\"frontend\":\"native\",\"nativeModule\":\"\",\"mediaBusy\":true}");
    assert(strstr(handoff->api.r[API_CONTROL].request,"POST /api/playback/stop "));assert(handoff->screen==SCREEN_HOME&&!api_busy(&handoff->api,API_BROWSE));
    handoff->home=1;api_cancel(&handoff->api,API_CONTROL);reply(handoff,API_STOP,"{\"playing\":false,\"transport\":{}}");assert(api_busy(&handoff->api,API_OPERATION));assert(handoff->screen==SCREEN_HOME);
    api_cancel(&handoff->api,API_OPERATION);reply(handoff,API_MODULE_OPEN,"{\"ready\":true,\"frontend\":\"native\",\"nativeModule\":\"\",\"mediaBusy\":false}");
    assert(!*handoff->opening_module&&handoff->screen==SCREEN_BROWSER&&!strcmp(handoff->active_module,target_id));
    app_free(handoff);app_init(handoff,1);app_fixture(handoff,"home-five");app_key(handoff,(KeyEvent){KEY_SELECT,1,0,0});api_cancel(&handoff->api,API_OPERATION);
    reply(handoff,API_MODULE_OPEN,"{\"ready\":true,\"frontend\":\"native\",\"nativeModule\":\"future-native\",\"mediaBusy\":true}");assert(strstr(handoff->api.r[API_CONTROL].request,"POST /api/modules/future-native/native/stop "));
    api_cancel(&handoff->api,API_CONTROL);reply(handoff,API_NATIVE_STOP,"{\"running\":false}");assert(api_busy(&handoff->api,API_OPERATION));api_cancel(&handoff->api,API_OPERATION);
    reply(handoff,API_MODULE_OPEN,"{\"ready\":true,\"frontend\":\"native\",\"nativeModule\":\"\",\"mediaBusy\":false}");assert(handoff->screen==SCREEN_BROWSER);
    app_free(handoff);app_init(handoff,1);app_fixture(handoff,"home-five");app_key(handoff,(KeyEvent){KEY_SELECT,1,0,0});api_cancel(&handoff->api,API_OPERATION);
    reply(handoff,API_MODULE_OPEN,"{\"ready\":true,\"frontend\":\"native\",\"nativeModule\":\"\",\"mediaBusy\":true}");ApiResponse refused={API_STOP,API_FAILED,handoff->api.generation,502,(const unsigned char *)"{}",2};api_cancel(&handoff->api,API_CONTROL);app_response(handoff,&refused);
    assert(!*handoff->opening_module&&handoff->screen==SCREEN_HOME&&!api_busy(&handoff->api,API_BROWSE)&&strstr(handoff->notice,"previous module"));
    app_free(handoff);app_init(handoff,1);app_fixture(handoff,"home-five");ModuleDescriptor *native=&handoff->modules.items[handoff->home_modules[0]];native->native_app=native->release_surface=native->release_input=1;
    app_key(handoff,(KeyEvent){KEY_SELECT,1,0,0});api_cancel(&handoff->api,API_OPERATION);reply(handoff,API_MODULE_OPEN,"{\"ready\":true,\"frontend\":\"native\",\"nativeModule\":\"\",\"mediaBusy\":true}");assert(!handoff->native_app_starting&&handoff->screen==SCREEN_HOME);
    reply(handoff,API_STATE,"{\"playing\":true,\"source\":\"camera\",\"generation\":1}");assert(handoff->screen==SCREEN_HOME);
    api_cancel(&handoff->api,API_CONTROL);reply(handoff,API_STOP,"{\"playing\":false,\"transport\":{}}");api_cancel(&handoff->api,API_OPERATION);reply(handoff,API_MODULE_OPEN,"{\"ready\":true,\"frontend\":\"native\",\"nativeModule\":\"\",\"mediaBusy\":false}");assert(handoff->native_app_starting&&handoff->screen==SCREEN_NATIVE_APP);
    app_free(handoff);app_init(handoff,1);app_fixture(handoff,"home-five");handoff->frontend_prepared=0;app_key(handoff,(KeyEvent){KEY_SELECT,1,0,0});api_cancel(&handoff->api,API_OPERATION);
    ApiResponse idle={API_MODULE_OPEN,API_OK,handoff->api.generation,200,(const unsigned char *)"{\"ready\":true,\"frontend\":\"native\",\"nativeModule\":\"\",\"mediaBusy\":false}",77};idle.length=strlen((const char *)idle.bytes);app_response(handoff,&idle);
    assert(handoff->opening_prepare&&handoff->screen==SCREEN_HOME&&!api_busy(&handoff->api,API_BROWSE));app_tick(handoff,ui_now());assert(handoff->api.r[API_CONTROL].kind==API_PREPARE);api_cancel(&handoff->api,API_CONTROL);
    ApiResponse busy={API_PREPARE,API_FAILED,handoff->api.generation,409,(const unsigned char *)"{}",2};app_response(handoff,&busy);assert(handoff->opening_prepare&&*handoff->opening_module&&!handoff->notice[0]&&handoff->prepare_due!=UINT64_MAX);
    ApiResponse prepared={API_PREPARE,API_OK,handoff->api.generation,200,(const unsigned char *)"{\"prepared\":true}",17};app_response(handoff,&prepared);assert(!*handoff->opening_module&&handoff->screen==SCREEN_BROWSER&&handoff->frontend_prepared);
    app_free(handoff);free(handoff);
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
    App *surf=calloc(1,sizeof *surf);assert(surf);app_init(surf,1);surf->screen=SCREEN_HIDDEN;surf->playback.playing=1;surf->playback.live=1;surf->playback.can_channel_up=surf->playback.can_channel_down=1;
    assert(input_key(0x1e006)==KEY_CHANNEL_UP&&input_key(0x1e007)==KEY_CHANNEL_DOWN);
    app_key(surf,(KeyEvent){KEY_CHANNEL_UP,1,0,0x1e006});assert(surf->screen==SCREEN_PLAYER&&surf->loading&&surf->channel_changing&&strstr(surf->api.r[API_OPERATION].request,"/api/playback/channelUp"));api_cancel(&surf->api,API_OPERATION);
    ApiResponse failure={API_PLAY,API_FAILED,surf->api.generation,502,NULL,0};app_response(surf,&failure);assert(!surf->awaiting&&surf->screen==SCREEN_PLAYER&&!surf->loading);
    app_key(surf,(KeyEvent){KEY_CHANNEL_DOWN,1,0,0x1e007});assert(strstr(surf->api.r[API_OPERATION].request,"/api/playback/channelDown"));api_cancel(&surf->api,API_OPERATION);app_key(surf,(KeyEvent){KEY_MENU,1,0,0});app_response(surf,&failure);assert(surf->screen==SCREEN_HOME&&!surf->channel_changing);app_free(surf);free(surf);
    app_free(app);free(app);return 0;
}
