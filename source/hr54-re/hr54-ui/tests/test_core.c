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
    /* Physical RC LEFT is also Back. Preserve only real horizontal controls. */
    App *left=calloc(1,sizeof(*left));assert(left);app_init(left,1);app_fixture(left,"jellyfin-browser");
    app_key(left,(KeyEvent){KEY_LEFT,1,1,0x1e102});assert(left->screen==SCREEN_BROWSER);key(left,KEY_LEFT);assert(left->screen==SCREEN_HOME);
    app_fixture(left,"jellyfin-browser");key(left,KEY_SELECT);assert(left->screen==SCREEN_DETAILS);key(left,KEY_LEFT);assert(left->screen==SCREEN_BROWSER);
    left->screen=SCREEN_PAIR;key(left,KEY_LEFT);assert(left->screen==SCREEN_HOME);
    left->screen=SCREEN_SETTINGS;left->settings_focus=1;key(left,KEY_LEFT);assert(left->screen==SCREEN_HOME);
    left->screen=SCREEN_PLAYER;left->playback.playing=1;key(left,KEY_LEFT);assert(left->screen==SCREEN_HIDDEN);
    left->screen=SCREEN_SETTINGS;left->settings_focus=0;left->quality_count=2;left->quality_rates[0]=1000;left->quality_rates[1]=2000;left->bitrate=2000;key(left,KEY_LEFT);assert(left->screen==SCREEN_SETTINGS&&left->bitrate==1000);
    left->screen=SCREEN_KEYBOARD;left->keyboard_focus=10;key(left,KEY_LEFT);assert(left->screen==SCREEN_KEYBOARD&&left->keyboard_focus==9);
    left->screen=SCREEN_HOME;left->home=0;key(left,KEY_LEFT);assert(left->screen==SCREEN_HOME&&left->home==4);app_free(left);free(left);
    App *app=calloc(1,sizeof(*app));assert(app);app_init(app,1);reply(app,API_READY,"{\"ready\":true,\"frontend\":\"native\",\"doomRunning\":false}");assert(app->screen==SCREEN_HOME&&!app->frontend_prepared);reply(app,API_PREPARE,"{\"prepared\":true}");assert(app->frontend_prepared);key(app,KEY_RIGHT);assert(app->home==1);key(app,KEY_LEFT);assert(app->home==0);key(app,KEY_RIGHT);key(app,KEY_RIGHT);key(app,KEY_RIGHT);key(app,KEY_RIGHT);assert(app->home==4);key(app,KEY_RIGHT);assert(app->home==0);
    app_fixture(app,"jellyfin-browser");BrowseNode *node=app_node(app);node->selection=3;key(app,KEY_SELECT);assert(app->screen==SCREEN_DETAILS);key(app,KEY_GUIDE);assert(app->screen==SCREEN_HOME&&node->selection==3);key(app,KEY_SELECT);assert(app->screen==SCREEN_BROWSER&&node->selection==3);key(app,KEY_SELECT);assert(app->screen==SCREEN_DETAILS);key(app,KEY_BACK);assert(app->screen==SCREEN_BROWSER);
    unsigned stale=app->api.generation;key(app,KEY_GUIDE);ApiResponse response={API_ITEMS,API_OK,stale,200,(const unsigned char *)"{\"items\":[]}",12};app_response(app,&response);assert(node->list.count==6&&app->screen==SCREEN_HOME);
    app->source=SOURCE_YOUTUBE;app->screen=SCREEN_KEYBOARD;app->query[0]=0;app->keyboard_focus=0;key(app,KEY_SELECT);assert(!strcmp(app->query,"a"));app->keyboard_focus=41;key(app,KEY_SELECT);assert(!strcmp(app->query,"a "));key(app,KEY_BACK);assert(!strcmp(app->query,"a"));app->keyboard_focus=43;key(app,KEY_SELECT);assert(!*app->query);
    app->screen=SCREEN_HOME;app->playback=(PlaybackState){0};app->dirty=0;reply(app,API_STATE,"{\"playing\":false,\"elapsed\":0,\"title\":\"\",\"source\":null,\"transport\":{}}");assert(!app->dirty);reply(app,API_STATE,"{\"playing\":true,\"elapsed\":10,\"title\":\"Movie\",\"source\":\"jellyfin\",\"transport\":{\"pause\":true}}");assert(app->dirty&&app->playback.can_pause);
    ApiResponse state_timeout={API_STATE,API_TIMEOUT,app->api.generation,0,NULL,0};app->notice[0]=0;app_response(app,&state_timeout);assert(!app->notice[0]&&app->status_failures==1);reply(app,API_STATE,"{\"playing\":true,\"elapsed\":11,\"title\":\"Movie\",\"source\":\"jellyfin\",\"transport\":{\"pause\":true}}");assert(!app->status_failures);
    app->screen=SCREEN_HOME;app->awaiting=API_PLAY;reply(app,API_PLAY,"{\"playing\":true}");assert(app->screen==SCREEN_HOME);/* GUIDE while preparing must stay visible. */
    app->screen=SCREEN_HOME;app->playback.playing=0;key(app,KEY_BACK);assert(app->screen==SCREEN_HOME);ApiResponse exit_failure={API_EXIT,API_UNAVAILABLE,app->api.generation,0,NULL,0};app_response(app,&exit_failure);assert(app->screen==SCREEN_HOME);key(app,KEY_EXIT);assert(app->screen==SCREEN_HOME);key(app,KEY_MENU);assert(app->screen==SCREEN_HOME&&!app->frontend_prepared);/* Idle native exit never exposes the underlying stock surface. */
    Artwork art={0};ui_copy(art.pending,sizeof(art.pending),"bad");assert(artwork_accept(&art,(const unsigned char *)"garbage",7,1)<0);assert(artwork_find(&art,"bad")->failed);artwork_free(&art);
    app_free(app);app_init(app,1);reply(app,API_READY,"{\"ready\":true,\"frontend\":\"native\",\"doomRunning\":true}");assert(app->doom_running&&app->screen==SCREEN_DOOM&&app->home==4);reply(app,API_DOOM_STATUS,"{\"running\":false,\"native\":true}");assert(app->screen==SCREEN_HOME&&!app->doom_running);
    app->source=SOURCE_IPTV;app->screen=SCREEN_BROWSER;reply(app,API_GROUPS,"{\"groups\":[{\"name\":\"News\",\"label\":\"News\"},{\"name\":\"\",\"label\":\"Other channels\"}],\"groupCount\":2}");node=app_node(app);assert(node->list.count==3&&node->list.item[0].virtual_entry);key(app,KEY_SELECT);assert(app_node(app)->kind==BROWSE_ITEMS&&!strstr(app->api.r[API_BROWSE].request,"group="));key(app,KEY_BACK);node->selection=1;key(app,KEY_SELECT);assert(strstr(app->api.r[API_BROWSE].request,"group=News&"));key(app,KEY_BACK);node->selection=2;key(app,KEY_SELECT);assert(strstr(app->api.r[API_BROWSE].request,"group=&"));key(app,KEY_BACK);key(app,KEY_INFO);ui_copy(app->query,sizeof(app->query),"news");app->keyboard_focus=44;key(app,KEY_SELECT);assert(strstr(app->api.r[API_BROWSE].request,"query=news&")&&!strstr(app->api.r[API_BROWSE].request,"group="));
    key(app,KEY_GUIDE);unsigned generation=app->api.generation;ApiResponse poll={API_POLL,API_OK,generation-1,200,(const unsigned char *)"{\"authenticated\":true}",22};app_response(app,&poll);assert(app->screen==SCREEN_HOME&&!app->awaiting);reply(app,API_PAIR,"{\"code\":\"123456\"}");assert(app->screen==SCREEN_HOME); /* Late approval/code cannot navigate away from home. */
    app_free(app);app_init(app,1);app->start_hidden=1;reply(app,API_READY,"{\"ready\":true,\"frontend\":\"native\",\"doomRunning\":false,\"mediaBusy\":true}");assert(app->screen==SCREEN_HIDDEN&&!app->start_hidden&&!app->frontend_prepared);
    /* Playback predates a transparent maintenance restart: stopping it must
       restore Home, never the already-completed startup screen. */
    reply(app,API_STATE,"{\"playing\":true,\"source\":\"jellyfin\",\"transport\":{}}");assert(app->screen==SCREEN_HIDDEN);
    reply(app,API_STATE,"{\"playing\":false,\"transport\":{}}");assert(app->screen==SCREEN_HOME&&app->ready&&!app->frontend_prepared);
    /* A saved media selection still wins when this process started playback. */
    app->screen=SCREEN_HIDDEN;app->frontend_prepared=1;app->return_screen=SCREEN_DETAILS;app->return_source=SOURCE_JELLYFIN;app->source=SOURCE_IPTV;app->sources[0].node[0].selection=3;
    reply(app,API_STATE,"{\"playing\":true,\"source\":\"jellyfin\",\"transport\":{}}");reply(app,API_STATE,"{\"playing\":false,\"transport\":{}}");assert(app->screen==SCREEN_DETAILS&&app->source==SOURCE_JELLYFIN&&app->sources[0].node[0].selection==3&&!app->frontend_prepared);
    /* An Android/API stop while native player controls are visible also
       restores browsing, instead of retaining a stale player/error overlay. */
    app->screen=SCREEN_PLAYER;app->playback.playing=1;app->loading=1;app->player_until=99999;
    reply(app,API_STATE,"{\"playing\":false,\"transport\":{}}");assert(app->screen==SCREEN_DETAILS&&!app->loading&&!app->player_until);
    /* Explicit STOP and state polling use the same restoration contract. */
    app->return_screen=SCREEN_STARTUP;app->screen=SCREEN_HIDDEN;reply(app,API_STOP,"{\"ok\":true}");assert(app->screen==SCREEN_HOME);
    key(app,KEY_MENU);assert(app->screen==SCREEN_HOME);
    app_free(app);free(app);puts("PASS JSON, Unicode, raw keys, ribbon, state restoration, stale replies, keyboard, idle frames, GUIDE during preparation, invalid artwork, Doom restart handoff, IPTV All/exact/empty/search routing, stale Quick Connect");return 0;
}
