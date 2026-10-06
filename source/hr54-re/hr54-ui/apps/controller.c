#include "state.h"
#include "../ui/animation.h"
const ModuleDescriptor *app_module(const App *a,const char *id){for(int i=0;i<a->modules.count;i++)if(!strcmp(a->modules.items[i].id,id))return &a->modules.items[i];return NULL;}
const ModuleDescriptor *app_home_module(const App *a,int position){return position>=0&&position<a->home_count?&a->modules.items[a->home_modules[position]]:NULL;}
void app_rebuild_home(App *a){char selected[64]="";const ModuleDescriptor *m=app_home_module(a,a->home);if(m)ui_copy(selected,sizeof selected,m->id);a->home_count=0;for(int i=0;i<a->modules.count;i++){m=&a->modules.items[i];if(m->installed&&m->enabled&&m->home)a->home_modules[a->home_count++]=i;}a->home=0;for(int i=0;i<a->home_count;i++)if(!strcmp(app_home_module(a,i)->id,selected))a->home=i;a->home_motion=0;}
BrowseNode *app_node(App *a){return &a->navigation[a->depth];}
const MediaItem *app_selected(App *a){BrowseNode *n=app_node(a);return n->selection>=0&&n->selection<a->media.count?&a->media.item[n->selection]:NULL;}
void app_art_key(char *out,size_t cap,const char *module,const char *art){snprintf(out,cap,"%s:%s:%s",art&&*art?"media":"icon",module,art?art:"");}
static void screen(App *a,Screen s){a->previous=a->screen;a->screen=s;a->visible=s!=SCREEN_HIDDEN&&s!=SCREEN_NATIVE_APP;a->dirty=1;}
static void notice(App *a,const char *text){ui_copy(a->notice,sizeof a->notice,text);a->notice_until=ui_now()+3500;a->dirty=1;}
static void invalidate(App *a){++a->api.generation;api_cancel(&a->api,API_BROWSE);api_cancel(&a->api,API_ARTWORK);a->artwork.pending[0]=0;a->loading=0;}
void app_init(App *a,int port){memset(a,0,sizeof *a);api_init(&a->api,port);a->screen=SCREEN_STARTUP;a->return_screen=SCREEN_HOME;a->visible=a->dirty=1;ui_copy(a->message,sizeof a->message,"Starting display");}
void app_free(App *a){api_close(&a->api);artwork_free(&a->artwork);}
void app_error(App *a,ApiError e){a->loading=0;a->error_return=a->screen;ui_copy(a->message,sizeof a->message,api_error_message(e));screen(a,SCREEN_ERROR);}
static void playback_return(App *a){Screen target=a->return_screen;if(target!=SCREEN_HOME&&target!=SCREEN_BROWSER&&target!=SCREEN_DETAILS)target=SCREEN_HOME;ui_copy(a->active_module,sizeof a->active_module,a->return_module);a->frontend_prepared=0;a->prepare_due=0;screen(a,target);}
void app_load(App *a){BrowseNode *n=app_node(a);invalidate(a);a->loading=1;screen(a,SCREEN_BROWSER);if(api_module_browse(&a->api,a->active_module,n->parent,n->query,n->offset))app_error(a,API_FAILED);}
static void module_settings(App *a,const char *id){invalidate(a);if(id!=a->managed_module)ui_copy(a->managed_module,sizeof a->managed_module,id);a->settings_focus=0;a->loading=1;screen(a,SCREEN_MODULE_SETTINGS);if(api_module_settings(&a->api,id))app_error(a,API_FAILED);}
static void keyboard(App *a,KeyboardMode mode){a->keyboard_return=a->screen;a->keyboard_mode=mode;a->keyboard_focus=0;if(mode==KEYBOARD_SEARCH)ui_copy(a->query,sizeof a->query,app_node(a)->query);screen(a,SCREEN_KEYBOARD);}
const char *app_keyboard_text(const App *a){return a->keyboard_mode==KEYBOARD_SEARCH?a->query:a->entry;}
const char *app_keyboard_letters(const App *a){if(a->keyboard_mode==KEYBOARD_SEARCH)return a->keyboard_upper?"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.'?":"abcdefghijklmnopqrstuvwxyz0123456789-.'?";return a->keyboard_upper?"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:/.-_?&=%#~+@":"abcdefghijklmnopqrstuvwxyz0123456789:/.-_?&=%#~+@";}
int app_keyboard_cells(const App *a){return a->keyboard_mode==KEYBOARD_SEARCH?40:50;}
static void browse_open(App *a){const ModuleDescriptor *m=app_module(a,a->active_module);if(!m)return;if(m->browse)app_load(a);else if(m->search){screen(a,SCREEN_HOME);keyboard(a,KEYBOARD_SEARCH);}else if(m->settings)module_settings(a,m->id);else notice(a,"This module has no browser.");}
static void open_module(App *a){const ModuleDescriptor *m=app_home_module(a,a->home);if(!m)return;if(api_busy(&a->api,API_OPERATION)){notice(a,"Please wait for the current operation.");return;}if(!m->healthy){notice(a,*m->error?m->error:"Module unavailable. Open Settings → Modules.");return;}
    invalidate(a);if(strcmp(a->active_module,m->id)){memset(a->navigation,0,sizeof a->navigation);memset(&a->media,0,sizeof a->media);a->depth=0;}ui_copy(a->active_module,sizeof a->active_module,m->id);ui_copy(a->navigation[0].title,sizeof a->navigation[0].title,m->name);
    if(m->native_app){ui_copy(a->active_native_module,sizeof a->active_native_module,m->id);a->native_release_surface=m->release_surface;a->native_release_input=m->release_input;a->native_app_starting=1;a->return_screen=SCREEN_HOME;screen(a,SCREEN_NATIVE_APP);return;}
    if(m->auth){char path[128];snprintf(path,sizeof path,"/api/modules/%s/status",m->id);a->loading=1;if(api_send(&a->api,API_BROWSE,API_MODULE_STATUS,"GET",path,NULL,120000))app_error(a,API_FAILED);}else browse_open(a);
}
static void push_node(App *a,const char *parent,const char *title,const char *query){if(a->depth+1>=NAV_DEPTH){notice(a,"This folder is too deep to open.");return;}BrowseNode *n=&a->navigation[++a->depth];memset(n,0,sizeof *n);ui_copy(n->parent,sizeof n->parent,parent);ui_copy(n->title,sizeof n->title,title);ui_copy(n->query,sizeof n->query,query);app_load(a);}
static void play(App *a){const MediaItem *item=app_selected(a);if(!item||!item->playable){notice(a,"This item is not available for playback.");return;}if(api_busy(&a->api,API_OPERATION)){notice(a,"Please wait for the current operation.");return;}a->return_screen=a->screen;ui_copy(a->return_module,sizeof a->return_module,a->active_module);ui_copy(a->operation_module,sizeof a->operation_module,a->active_module);a->loading=1;a->cancel_play=0;a->awaiting=API_PLAY;screen(a,SCREEN_PLAYER);ui_copy(a->playback.title,sizeof a->playback.title,item->title);if(api_module_play(&a->api,a->active_module,item))app_error(a,API_FAILED);}
static void submit_entry(App *a){if(a->keyboard_mode==KEYBOARD_SEARCH){if(!*a->query){notice(a,"Enter something to search for.");return;}push_node(a,"","Search results",a->query);}
    else if(a->keyboard_mode==KEYBOARD_URL){a->loading=1;screen(a,SCREEN_MODULES);if(api_module_install(&a->api,a->entry))app_error(a,API_FAILED);}
    else {ModuleField *f=&a->module_settings.fields[a->edit_field];ui_copy(f->value,sizeof f->value,a->entry);a->loading=1;screen(a,SCREEN_MODULE_SETTINGS);if(api_module_save(&a->api,a->managed_module,f))app_error(a,API_FAILED);}}
static void keyboard_key(App *a,KeyEvent ev){int cells=app_keyboard_cells(a),focus=a->keyboard_focus;char *text=a->keyboard_mode==KEYBOARD_SEARCH?a->query:a->entry;size_t cap=a->keyboard_mode==KEYBOARD_SEARCH?sizeof a->query:a->keyboard_mode==KEYBOARD_FIELD?256:sizeof a->entry;
    if(ev.key==KEY_UP||ev.key==KEY_DOWN||ev.key==KEY_LEFT||ev.key==KEY_RIGHT){int delta=ev.key==KEY_LEFT?-1:ev.key==KEY_RIGHT?1:ev.key==KEY_UP?-10:10;a->keyboard_focus=(focus+delta+cells+10)%(cells+10);return;}
    if(ev.key==KEY_BACK||(ev.key==KEY_SELECT&&focus==cells+2)){size_t n=strlen(text);if(n){do{--n;}while(n&&((unsigned char)text[n]&192)==128);text[n]=0;}else if(ev.key==KEY_BACK)screen(a,a->keyboard_return);return;}
    if(ev.key!=KEY_SELECT)return;if(focus<cells){const char *letters=app_keyboard_letters(a);size_t n=strlen(text);if((size_t)focus<strlen(letters)&&n+1<cap){text[n]=letters[focus];text[n+1]=0;}}
    else if(focus==cells)a->keyboard_upper=!a->keyboard_upper;
    else if(focus==cells+1&&a->keyboard_mode!=KEYBOARD_URL){size_t n=strlen(text);if(n+1<cap){text[n]=' ';text[n+1]=0;}}
    else if(focus==cells+3)text[0]=0;else if(focus>=cells+4&&focus<=cells+7)submit_entry(a);else if(focus>=cells+8)screen(a,a->keyboard_return);
}
static void settings_key(App *a,KeyEvent ev){int count=0;
    if(a->screen==SCREEN_SETTINGS)count=3;else if(a->screen==SCREEN_MODULES)count=a->modules.count+2;else if(a->screen==SCREEN_MODULE_DETAIL){const ModuleDescriptor *m=app_module(a,a->managed_module);count=m&&m->installed?2+(m->enabled&&m->healthy&&m->settings):1;}else if(a->screen==SCREEN_MODULE_SETTINGS)count=a->module_settings.field_count+a->module_settings.action_count;
    if(ev.key==KEY_BACK){invalidate(a);screen(a,a->screen==SCREEN_SETTINGS?SCREEN_HOME:a->screen==SCREEN_MODULES||a->screen==SCREEN_ABOUT?SCREEN_SETTINGS:a->screen==SCREEN_MODULE_DETAIL?SCREEN_MODULES:SCREEN_MODULE_DETAIL);a->settings_focus=0;return;}
    if(a->loading)return;if(ev.key==KEY_UP||ev.key==KEY_DOWN){if(count)a->settings_focus=(a->settings_focus+(ev.key==KEY_DOWN?1:count-1))%count;return;}
    if(a->screen==SCREEN_MODULE_SETTINGS&&a->settings_focus<a->module_settings.field_count&&(ev.key==KEY_RIGHT||ev.key==KEY_LEFT)){
        ModuleField *f=&a->module_settings.fields[a->settings_focus];if(f->type==FIELD_BOOL)ui_copy(f->value,sizeof f->value,!strcmp(f->value,"true")?"false":"true");
        if(f->type==FIELD_CHOICE){int current=0;for(int i=0;i<f->choice_count;i++)if(!strcmp(f->value,f->choices[i].value)&&f->quoted==f->choices[i].quoted)current=i;current=(current+(ev.key==KEY_RIGHT?1:f->choice_count-1))%f->choice_count;ui_copy(f->value,sizeof f->value,f->choices[current].value);f->quoted=f->choices[current].quoted;}return;
    }
    if(ev.key!=KEY_SELECT)return;
    if(a->screen==SCREEN_SETTINGS){if(a->settings_focus==0){screen(a,SCREEN_MODULES);a->refresh_modules=1;a->settings_focus=0;}else screen(a,a->settings_focus==1?SCREEN_ABOUT:SCREEN_HOME);}
    else if(a->screen==SCREEN_MODULES){if(a->settings_focus<a->modules.count){ui_copy(a->managed_module,sizeof a->managed_module,a->modules.items[a->settings_focus].id);a->module_focus=a->settings_focus;a->settings_focus=0;screen(a,SCREEN_MODULE_DETAIL);}else if(a->settings_focus==a->modules.count){ui_copy(a->entry,sizeof a->entry,"https://");keyboard(a,KEYBOARD_URL);}else{a->management_pair=1;a->poll_action[0]=a->pair_code[0]=0;ui_copy(a->message,sizeof a->message,"Enter this temporary code in Android module management. It expires in two minutes.");screen(a,SCREEN_PAIR);a->loading=1;if(api_management_pair_open(&a->api))app_error(a,API_FAILED);}}
    else if(a->screen==SCREEN_MODULE_DETAIL){const ModuleDescriptor *m=app_module(a,a->managed_module);if(!m)return;if(m->installed&&a->settings_focus==2){module_settings(a,m->id);return;}const char *operation=!m->installed?"reinstall":a->settings_focus==1?"uninstall":m->enabled?"disable":"enable";a->loading=1;if(api_module_manage(&a->api,m->id,operation))app_error(a,API_FAILED);}
    else if(a->screen==SCREEN_MODULE_SETTINGS){if(a->settings_focus<a->module_settings.field_count){ModuleField *f=&a->module_settings.fields[a->settings_focus];if(f->type==FIELD_STRING||f->type==FIELD_INTEGER){a->edit_field=a->settings_focus;ui_copy(a->entry,sizeof a->entry,f->value);keyboard(a,KEYBOARD_FIELD);}else{a->loading=1;if(api_module_save(&a->api,a->managed_module,f))app_error(a,API_FAILED);}}
        else{int index=a->settings_focus-a->module_settings.field_count;if(index<a->module_settings.action_count){a->management_pair=0;a->poll_action[0]=a->pair_code[0]=a->message[0]=0;a->loading=1;screen(a,SCREEN_PAIR);if(api_module_action(&a->api,a->managed_module,a->module_settings.actions[index].id))app_error(a,API_FAILED);}}}
}
void app_key(void *ctx,KeyEvent ev){App *a=ctx;if(!ev.pressed)return;
    int can_adjust=a->screen==SCREEN_MODULE_SETTINGS&&a->settings_focus<a->module_settings.field_count&&(a->module_settings.fields[a->settings_focus].type==FIELD_CHOICE||a->module_settings.fields[a->settings_focus].type==FIELD_BOOL);
    if(ev.key==KEY_LEFT&&!can_adjust&&(a->screen==SCREEN_BROWSER||a->screen==SCREEN_DETAILS||a->screen==SCREEN_PLAYER||a->screen==SCREEN_PAIR||a->screen==SCREEN_ERROR||a->screen==SCREEN_SETTINGS||a->screen>=SCREEN_MODULES))ev.key=KEY_BACK;
    uint64_t now=ui_now();int direction=ev.key==KEY_UP||ev.key==KEY_DOWN||ev.key==KEY_LEFT||ev.key==KEY_RIGHT;if(ev.repeat&&(!direction||now-a->last_key<90))return;a->last_key=now;a->dirty=1;
    if(ev.key==KEY_GUIDE||ev.key==KEY_MENU){a->revealed_play_generation=a->playback.generation;a->frontend_prepared=0;a->prepare_due=0;invalidate(a);a->refresh_modules=1;if(a->native_app_starting){a->native_app_starting=0;screen(a,SCREEN_HOME);}else if(a->native_app_running||a->api.r[API_OPERATION].kind==API_NATIVE_START){a->native_app_returning=1;if(a->native_app_running)api_module_native(&a->api,a->active_native_module,"stop");}else screen(a,SCREEN_HOME);return;}
    if(ev.key==KEY_EXIT){screen(a,a->playback.playing?SCREEN_HIDDEN:SCREEN_HOME);api_tv_exit(&a->api);return;}
    if(ev.key==KEY_STOP){if(a->native_app_running){a->native_app_returning=1;api_module_native(&a->api,a->active_native_module,"stop");}else if(a->playback.playing||a->awaiting==API_PLAY){a->cancel_play=1;api_playback_stop(&a->api);}return;}
    if((ev.key==KEY_PLAY||ev.key==KEY_PAUSE)&&a->playback.playing){int resume=a->playback.paused;if(ev.key==KEY_PLAY&&!resume)return;if(resume?a->playback.can_resume:a->playback.can_pause){if(!api_playback_pause(&a->api,resume)){a->loading=1;a->player_until=now+5000;screen(a,SCREEN_PLAYER);}}else notice(a,"This transport operation is unavailable.");return;}
    if((ev.key==KEY_FORWARD||ev.key==KEY_REWIND||ev.key==KEY_SKIP_FORWARD||ev.key==KEY_SKIP_BACK)&&a->playback.playing){if(a->playback.can_seek){a->loading=1;api_playback_seek(&a->api,ev.key==KEY_FORWARD||ev.key==KEY_SKIP_FORWARD?30:ev.key==KEY_SKIP_BACK?-10:-30);a->player_until=now+5000;screen(a,SCREEN_PLAYER);}else notice(a,"Seeking is unavailable.");return;}
    if(ev.key==KEY_INFO&&a->playback.playing){a->player_until=now+5000;screen(a,SCREEN_PLAYER);return;}
    if(a->screen==SCREEN_STARTUP)return;
    if(a->screen==SCREEN_HOME){if((ev.key==KEY_LEFT||ev.key==KEY_RIGHT)&&a->home_count){int delta=ev.key==KEY_RIGHT?1:-1;a->home_motion=a->home_motion*(1000-ui_transition_progress(a->animate_start,now))/1000+delta*1000;a->home_from=a->home;a->home=(a->home+delta+a->home_count)%a->home_count;a->home_direction=delta;a->animate_start=now;}else if(ev.key==KEY_DOWN||ev.key==KEY_UP){a->settings_focus=0;screen(a,SCREEN_SETTINGS);}else if(ev.key==KEY_SELECT)open_module(a);else if(ev.key==KEY_BACK){screen(a,a->playback.playing?SCREEN_HIDDEN:SCREEN_HOME);api_tv_exit(&a->api);}return;}
    if(a->screen==SCREEN_HIDDEN){if(ev.key==KEY_INFO)screen(a,SCREEN_PLAYER);return;}
    if(a->screen==SCREEN_KEYBOARD){keyboard_key(a,ev);return;}
    if(a->screen==SCREEN_SETTINGS||a->screen>=SCREEN_MODULES){settings_key(a,ev);return;}
    if(a->screen==SCREEN_ERROR){if(ev.key==KEY_BACK)screen(a,a->error_return==SCREEN_STARTUP?SCREEN_HOME:a->error_return);else if(ev.key==KEY_INFO){const ModuleDescriptor *m=app_module(a,a->active_module);if(m&&m->settings)module_settings(a,m->id);}else if(ev.key==KEY_SELECT){if(a->error_return==SCREEN_BROWSER)app_load(a);else if(a->error_return==SCREEN_MODULE_SETTINGS)module_settings(a,a->managed_module);else{a->refresh_modules=1;screen(a,SCREEN_HOME);}}return;}
    if(a->screen==SCREEN_PAIR){if(ev.key==KEY_BACK){invalidate(a);a->poll_action[0]=0;screen(a,a->management_pair?SCREEN_MODULES:SCREEN_MODULE_SETTINGS);}return;}
    if(a->screen==SCREEN_PLAYER){if(ev.key==KEY_BACK){if(a->loading&&a->awaiting==API_PLAY){a->cancel_play=1;api_playback_stop(&a->api);screen(a,a->return_screen);}else screen(a,SCREEN_HIDDEN);}else if(ev.key==KEY_SELECT&&a->playback.playing&&a->playback.can_stop){a->loading=1;api_playback_stop(&a->api);}return;}
    if(a->screen==SCREEN_DETAILS){if(ev.key==KEY_BACK)screen(a,SCREEN_BROWSER);else if(ev.key==KEY_SELECT)play(a);return;}
    if(a->screen==SCREEN_BROWSER){BrowseNode *n=app_node(a);const ModuleDescriptor *m=app_module(a,a->active_module);if(ev.key==KEY_BACK){invalidate(a);if(a->depth){a->depth--;app_load(a);}else screen(a,SCREEN_HOME);return;}if(a->loading)return;
        if(ev.key==KEY_INFO||ev.key==KEY_RIGHT){if(m&&m->search)keyboard(a,KEYBOARD_SEARCH);return;}
        if(ev.key==KEY_UP||ev.key==KEY_DOWN){int next=n->selection+(ev.key==KEY_DOWN?1:-1);if(next>=a->media.count&&a->media.has_more){n->offset=a->media.offset+a->media.count;n->selection=0;app_load(a);}else if(next<0&&n->offset>0){n->offset-=UI_PAGE_SIZE;if(n->offset<0)n->offset=0;n->selection=0;app_load(a);}else if(next>=0&&next<a->media.count){n->selection=next;a->art_due=now+120;}return;}
        if(ev.key==KEY_SELECT){const MediaItem *x=app_selected(a);if(!x)return;if(x->folder){char id[256],title[256];ui_copy(id,sizeof id,x->id);ui_copy(title,sizeof title,x->title);push_node(a,id,title,"");}else if(!strcmp(x->type,"action")){ui_copy(a->managed_module,sizeof a->managed_module,a->active_module);a->management_pair=0;screen(a,SCREEN_PAIR);a->loading=1;if(api_module_action(&a->api,a->active_module,x->id))app_error(a,API_FAILED);}else if(x->playable)screen(a,SCREEN_DETAILS);else notice(a,"This item is unavailable.");}return;}
}
void app_response(void *ctx,const ApiResponse *r){App *a=ctx;
    if(r->kind==API_IMAGE){if(r->generation==a->api.generation&&artwork_queue(&a->artwork,r->bytes,r->length,r->error==API_OK,r->generation)){artwork_accept(&a->artwork,NULL,0,0);a->dirty=1;}return;}
    int browse=r->kind==API_MODULES||r->kind==API_MEDIA||r->kind==API_MODULE_STATUS||r->kind==API_MODULE_SETTINGS||r->kind==API_MODULE_ACTION||r->kind==API_MANAGEMENT_PAIR;
    if(browse&&r->generation!=a->api.generation)return;
    ApiError error=r->error;OperationResult operation={0};PlaybackState playback={0};ModuleList *modules=NULL;
    if(error==API_OK){int rc=0;
        if(r->kind==API_MODULES){modules=calloc(1,sizeof *modules);rc=modules?api_parse_modules(r,modules):-1;}
        else if(r->kind==API_MEDIA)rc=api_parse_media(r,&a->media);
        else if(r->kind==API_MODULE_SETTINGS||r->kind==API_MODULE_SAVE)rc=api_parse_settings(r,&a->module_settings);
        else if(r->kind==API_STATE||r->kind==API_PLAY||r->kind==API_PAUSE||r->kind==API_SEEK)rc=api_parse_playback(r,&playback);
        else rc=api_parse_operation(r,&operation);
        if(rc)error=API_MALFORMED;
    }
    if(error!=API_OK){free(modules);
        if(r->kind==API_STATE){if(a->status_failures<6)a->status_failures++;a->next_status=ui_now()+(uint64_t)(1<<a->status_failures)*1000;return;}
        if(r->kind==API_READY){a->next_status=ui_now()+2000;ui_copy(a->message,sizeof a->message,"Waiting for the receiver service");a->dirty=1;return;}
        if(r->kind==API_PREPARE){a->frontend_prepared=0;a->prepare_due=UINT64_MAX;notice(a,"Preparing remote control. Press MENU to retry.");return;}
        if(r->kind==API_MODULES&&a->modules_loaded){notice(a,"Could not refresh modules.");return;}
        if(r->kind==API_NATIVE_STATUS||r->kind==API_EXIT){notice(a,api_error_message(error));return;}
        if(r->kind==API_NATIVE_START||r->kind==API_NATIVE_STOP){a->native_app_starting=0;a->native_app_running=1;a->native_app_returning=1;a->next_status=0;screen(a,SCREEN_NATIVE_APP);notice(a,api_error_message(error));return;}
        if(r->kind==API_PLAY)a->awaiting=0;app_error(a,error);
        Json j;if(!json_open(&j,(const char *)r->bytes,r->length)){char message[256];if(!json_string(&j,json_field(&j,0,"error"),message,sizeof message))ui_copy(a->message,sizeof a->message,message);json_close(&j);}return;
    }
    if(r->kind!=API_STATE)a->dirty=1;
    switch(r->kind){
    case API_READY:a->ready=operation.ready;a->refresh_modules=1;
        if(*operation.native_module){ui_copy(a->active_native_module,sizeof a->active_native_module,operation.native_module);a->native_app_running=1;a->native_release_input=a->native_release_surface=1;screen(a,SCREEN_NATIVE_APP);}
        else{screen(a,a->start_hidden?SCREEN_HIDDEN:SCREEN_HOME);a->start_hidden=0;}break;
    case API_PREPARE:a->frontend_prepared=operation.prepared;break;
    case API_MODULES:{char selected[64]="";const ModuleDescriptor *old=app_home_module(a,a->home);if(old)ui_copy(selected,sizeof selected,old->id);if(memcmp(&a->modules,modules,sizeof *modules)){artwork_reset(&a->artwork);api_cancel(&a->api,API_ARTWORK);++a->api.generation;}a->modules=*modules;a->home_count=0;app_rebuild_home(a);for(int i=0;i<a->home_count;i++)if(!strcmp(app_home_module(a,i)->id,selected))a->home=i;a->modules_loaded=1;a->loading=0;a->next_modules=ui_now()+5000;if(a->screen==SCREEN_MODULES&&a->settings_focus>=a->modules.count+2)a->settings_focus=0;break;}
    case API_MODULE_STATUS:a->loading=0;if(operation.authenticated){a->awaiting=API_MEDIA;}else{ui_copy(a->managed_module,sizeof a->managed_module,a->active_module);a->awaiting=API_MODULE_SETTINGS;}break;
    case API_MEDIA:{BrowseNode *n=app_node(a);a->loading=0;n->offset=a->media.offset;if(n->selection>=a->media.count)n->selection=a->media.count?a->media.count-1:0;a->art_due=ui_now()+100;break;}
    case API_STATE:{a->status_failures=0;if(strcmp(a->playback.instance,playback.instance)){a->observed_play_generation=0;a->revealed_play_generation=0;}if(playback.generation<a->observed_play_generation)break;int ended=a->playback.playing&&!playback.playing;int committed=playback.playing&&playback.generation>a->observed_play_generation;
        if(committed){a->observed_play_generation=playback.generation;if(!a->awaiting&&(a->screen==SCREEN_HOME||a->screen==SCREEN_BROWSER||a->screen==SCREEN_DETAILS)){a->return_screen=a->screen;ui_copy(a->return_module,sizeof a->return_module,a->active_module);}if(a->revealed_play_generation!=playback.generation)screen(a,SCREEN_HIDDEN);}
        if(memcmp(&a->playback,&playback,sizeof playback))a->dirty=1;a->playback=playback;
        if(ended&&(a->screen==SCREEN_HIDDEN||a->screen==SCREEN_PLAYER)&&a->awaiting!=API_PLAY){a->loading=0;a->player_until=0;playback_return(a);}break;}
    case API_PLAY:a->awaiting=0;a->loading=0;a->playback=playback;a->observed_play_generation=playback.generation;if(a->cancel_play)a->stop_after_play=1;else if(a->screen==SCREEN_PLAYER)screen(a,SCREEN_HIDDEN);a->next_status=0;break;
    case API_STOP:a->loading=0;if(a->awaiting!=API_PLAY){a->playback.playing=0;playback_return(a);}break;
    case API_PAUSE:case API_SEEK:a->loading=0;a->playback=playback;a->next_status=0;break;
    case API_MODULE_SETTINGS:case API_MODULE_SAVE:a->loading=0;if(r->kind==API_MODULE_SAVE)notice(a,"Module settings saved.");break;
    case API_MODULE_MUTATE:a->loading=0;a->refresh_modules=1;a->settings_focus=0;screen(a,SCREEN_MODULES);notice(a,"Module updated.");break;
    case API_MANAGEMENT_PAIR:a->loading=0;ui_copy(a->pair_code,sizeof a->pair_code,operation.code);break;
    case API_MODULE_ACTION:a->loading=0;ui_copy(a->pair_code,sizeof a->pair_code,operation.code);if(*operation.message)ui_copy(a->message,sizeof a->message,operation.message);if(*operation.poll_action)ui_copy(a->poll_action,sizeof a->poll_action,operation.poll_action);a->next_action=ui_now()+2000;if(operation.authenticated||(!operation.pending&&!*operation.code)){a->poll_action[0]=0;a->awaiting=API_MODULE_SETTINGS;}break;
    case API_NATIVE_START:a->native_app_starting=0;a->native_app_running=operation.running;if(operation.running)screen(a,SCREEN_NATIVE_APP);else{a->native_app_returning=0;screen(a,SCREEN_HOME);notice(a,"Module did not start.");}a->next_status=0;break;
    case API_NATIVE_STATUS:case API_NATIVE_STOP:if(!operation.running){a->native_app_running=a->native_app_starting=a->native_app_returning=0;a->active_native_module[0]=0;a->frontend_prepared=0;a->prepare_due=0;screen(a,SCREEN_HOME);}break;
    default:break;
    }
    free(modules);
}
int app_animating(const App *a,uint64_t now){return a->screen==SCREEN_STARTUP||(a->screen==SCREEN_HOME&&a->animate_start&&now-a->animate_start<UI_TRANSITION_MS);}
static void request_image(App *a,const char *module,const char *art){char key[384];app_art_key(key,sizeof key,module,art);if(artwork_find(&a->artwork,key))return;ui_copy(a->artwork.pending,sizeof a->artwork.pending,key);a->artwork.pending_icon=!art||!*art;if(api_module_image(&a->api,module,art))a->artwork.pending[0]=0;}
void app_tick(App *a,uint64_t now){if(artwork_poll(&a->artwork,a->api.generation))a->dirty=1;api_pump(&a->api,now,app_response,a);
    if(!api_busy(&a->api,API_BROWSE)){if(a->awaiting==API_MEDIA){a->awaiting=0;browse_open(a);}else if(a->awaiting==API_MODULE_SETTINGS){a->awaiting=0;module_settings(a,a->managed_module);}
        else if(a->ready&&(a->refresh_modules||((a->screen==SCREEN_HOME||a->screen==SCREEN_MODULES)&&now>=a->next_modules))){a->refresh_modules=0;a->next_modules=now+5000;api_modules(&a->api);}
        else if(a->screen==SCREEN_PAIR&&*a->poll_action&&now>=a->next_action){a->next_action=now+2000;api_module_action(&a->api,a->managed_module,a->poll_action);}}
    if(a->native_app_returning&&a->native_app_running&&!api_busy(&a->api,API_CONTROL)&&!api_busy(&a->api,API_OPERATION)){if(!api_module_native(&a->api,a->active_native_module,"stop"))a->native_app_returning=0;}
    if(a->stop_after_play&&!api_busy(&a->api,API_CONTROL)){a->stop_after_play=0;api_playback_stop(&a->api);}
    if(a->notice_until&&now>=a->notice_until){a->notice[0]=0;a->notice_until=0;a->dirty=1;}
    if(a->screen==SCREEN_PLAYER&&a->player_until&&now>=a->player_until&&!a->loading){a->player_until=0;screen(a,a->playback.playing?SCREEN_HIDDEN:SCREEN_HOME);}
    if(a->ready&&!a->frontend_prepared&&!a->native_app_running&&a->screen!=SCREEN_NATIVE_APP&&a->screen!=SCREEN_HIDDEN&&now>=a->prepare_due&&!api_busy(&a->api,API_CONTROL)){a->prepare_due=UINT64_MAX;if(api_system_prepare(&a->api)){a->prepare_due=now+2000;notice(a,"Preparing remote control.");}}
    if(now>=a->next_status&&!api_busy(&a->api,API_STATUS)){a->next_status=now+1000;if(!a->ready)api_system_ready(&a->api);else if(a->native_app_running)api_module_native(&a->api,a->active_native_module,"status");else api_playback_state(&a->api);}
    if(a->artwork.job||api_busy(&a->api,API_ARTWORK)||now<a->art_due)return;
    if((a->screen==SCREEN_BROWSER||a->screen==SCREEN_DETAILS)&&!a->loading){const MediaItem *x=app_selected(a);if(x&&*x->artwork)request_image(a,a->active_module,x->artwork);}
    else if(a->screen==SCREEN_HOME&&a->home_count){const int delta[]={0,-1,1,-2,2};for(size_t i=0;i<sizeof delta/sizeof delta[0]&&!api_busy(&a->api,API_ARTWORK);i++){int position=(a->home+delta[i]+a->home_count)%a->home_count;const ModuleDescriptor *m=app_home_module(a,position);if(m)request_image(a,m->id,NULL);}}
}
