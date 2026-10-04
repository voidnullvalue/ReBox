#include "platform.h"
#include "apps/state.h"
#include "input/dispatcher.h"
#include "ui/screens.h"
#ifdef HR54_RECEIVER
#include "native_egl.h"
#include "presentation_policy.h"
#endif
static volatile sig_atomic_t quit;
static void stop(int sig){(void)sig;quit=1;}
static void controller_key(void *ctx,KeyEvent ev){App *a=ctx;const char *trace=getenv("HR54_INPUT_TRACE");int enabled=trace&&strcmp(trace,"0");if(enabled)fprintf(stderr,"controller-trace: received raw=%08x UiKey=%d pressed=%d repeat=%d screen=%d home=%d\n",ev.raw,ev.key,ev.pressed,ev.repeat,a->screen,a->home);app_key(ctx,ev);if(enabled)fprintf(stderr,"controller-trace: completed UiKey=%d screen=%d home=%d loading=%d\n",ev.key,a->screen,a->home,a->loading);}
typedef struct {int done,ready,idle;} ApiCheck;
static void check_response(void *ctx,const ApiResponse *r){ApiCheck *check=ctx;ApiModel *m=calloc(1,sizeof(*m));check->done=1;if(m){check->ready=r->error==API_OK&&!api_model(r,m)&&(r->kind==API_STOP||(r->kind==API_PREPARE&&m->prepared)||(m->ready&&(!check->idle||!m->busy)));free(m);}}
static int check_api(int port,int idle){ApiClient client;ApiCheck check={.idle=idle>0};api_init(&client,port);if(idle==-2?api_system_prepare(&client):idle<0?api_playback_stop(&client,SOURCE_JELLYFIN):api_system_ready(&client)){api_close(&client);return 1;}uint64_t end=ui_now()+(idle==-2?18000:idle<0?31000:6500);while(!check.done&&ui_now()<end){api_pump(&client,ui_now(),check_response,&check);struct timespec d={0,20000000};nanosleep(&d,NULL);}api_close(&client);return check.done&&check.ready?0:1;}
#ifndef HR54_RECEIVER
static int snapshot(const char *path,const UiFramebuffer *f){FILE *out=fopen(path,"wb");if(!out)return -1;size_t n=fwrite(f->pixels,1,f->stride*f->height,out);int rc=fclose(out);return n==f->stride*f->height&&!rc?0:-1;}
static UiKey host_key(char c){switch(c){case 'w':return KEY_UP;case 's':return KEY_DOWN;case 'a':return KEY_LEFT;case 'd':return KEY_RIGHT;case '\n':case ' ':return KEY_SELECT;case 'b':return KEY_BACK;case 'g':return KEY_GUIDE;case 'i':return KEY_INFO;case 'p':return KEY_PAUSE;case 'x':return KEY_STOP;case 'f':return KEY_FORWARD;case 'r':return KEY_REWIND;default:return KEY_NONE;}}
#endif
int main(int argc,char **argv){int port=8130,manual=0,probe=0,api_check=0,idle_check=0,still=0,start_hidden=0;uint64_t preview_ms=0;unsigned seconds=0;const char *output=NULL;
#ifndef HR54_RECEIVER
    const char *preview=NULL;
#endif
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--manual"))manual=1;
        else if(!strcmp(argv[i],"--still"))still=1;
        else if(!strcmp(argv[i],"--start-hidden"))start_hidden=1;
        else if(!strcmp(argv[i],"--preview-ms")&&i+1<argc)preview_ms=(unsigned)atoi(argv[++i]);
        else if(!strcmp(argv[i],"--guide"))manual=2;
        else if(!strcmp(argv[i],"--probe-input"))probe=1;
        else if(!strcmp(argv[i],"--probe-handoff"))probe=2;
        else if(!strcmp(argv[i],"--check-api"))api_check=1;
        else if(!strcmp(argv[i],"--stop-media"))api_check=2;
        else if(!strcmp(argv[i],"--prepare-shell"))api_check=3;
        else if(!strcmp(argv[i],"--idle"))idle_check=1;
        else if(!strcmp(argv[i],"--seconds")&&i+1<argc)seconds=(unsigned)atoi(argv[++i]);
        else if(!strcmp(argv[i],"--port")&&i+1<argc)port=atoi(argv[++i]);
#ifndef HR54_RECEIVER
        else if(!strcmp(argv[i],"--preview")&&i+1<argc)preview=argv[++i];
        else if(!strcmp(argv[i],"--output")&&i+1<argc)output=argv[++i];

#endif
        else {fprintf(stderr,"hr54-ui: unsupported argument %s\n",argv[i]);return 2;}}
    if(port<1||port>65535||seconds>3600)return 2;
    if(idle_check&&!api_check)return 2;
    if(api_check)return check_api(port,api_check==3?-2:api_check==2?-1:idle_check);
    uint64_t runtime_end=seconds?ui_now()+seconds*1000:0;
#ifdef HR54_RECEIVER
    if(probe){Input input={.fd=-1};if(input_open(&input,manual))return 1;input.wanted_active=0;uint64_t deadline=ui_now()+12000;int rc=1,step=0;uint64_t settled=0;const int modes[]={1,2,1,0,1};while(ui_now()<deadline){if(input_pump(&input,ui_now(),NULL,NULL)<0)break;if(input.ready){if(probe==2&&step<5){if(input_mode(&input,modes[step++]))break;}else if(probe!=2||(settled&&ui_now()-settled>=2500)){fprintf(stderr,"hr54-ui: %s ownership accepted\n",probe==2?"handoff":"trigger");rc=0;break;}else if(!settled)settled=ui_now();}struct timespec d={0,20000000};nanosleep(&d,NULL);}input_close(&input);return rc;}
#else
    (void)probe;
#endif
#ifdef HR54_RECEIVER
    int lockfd=open("/tmp/hr54-ui.lock",O_CREAT|O_WRONLY,0600);
    if(lockfd<0||flock(lockfd,6)){fprintf(stderr,"hr54-ui: another instance owns the shell\n");if(lockfd>=0)close(lockfd);return 1;}
    fcntl(lockfd,F_SETFD,FD_CLOEXEC);
#endif
    App *app=calloc(1,sizeof(*app));UiFramebuffer frame;if(!app||ui_fb_init(&frame,UI_WIDTH,UI_HEIGHT)){free(app);return 1;}app_init(app,port);app->still=still;app->start_hidden=start_hidden;
#ifndef HR54_RECEIVER
    (void)manual;
    if(preview){if(!output){fprintf(stderr,"--preview requires --output\n");app_free(app);free(app);ui_fb_free(&frame);return 2;}app_fixture(app,preview);render_screen(&frame,app,preview_ms);int rc=snapshot(output,&frame);app_free(app);free(app);ui_fb_free(&frame);return rc?1:0;}
    int flags=fcntl(STDIN_FILENO,F_GETFL,0);fcntl(STDIN_FILENO,F_SETFL,flags|O_NONBLOCK);
#else
    (void)output;Hr54EglSurface surface={0};Input input={.fd=-1};int input_started=0,owned_once=0,last_hidden=-1;
    /* Readiness includes native-process lifecycle state. Do not open a
     * competing retained surface when restarting while Doom still owns it. */
#endif
    (void)preview_ms;uint64_t background_due=0;
#ifdef HR54_RECEIVER
    int home_submitted=0;uint64_t render_key_reported=0;
#endif
    signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);int result=0;
    while(!quit){uint64_t now=ui_now();app_tick(app,now);if(!app->still&&app->ready&&app->frontend_prepared&&app->screen==SCREEN_HOME&&now>=background_due){background_due=now+UI_BACKGROUND_INTERVAL;app->dirty=1;}
#ifdef HR54_RECEIVER
        if(app->ready&&!input_started){const char *direct=getenv("HR54_DIRECT_INPUT");int rc=direct&&!strcmp(direct,"1")?input_open(&input,manual==1?1:2):input_open_broker(&input,manual);if(rc)goto failed;input.wanted_active=0;input_started=1;}
        if(input_started&&input_pump(&input,now,controller_key,app)<0)goto failed;
        if(input.ready&&!owned_once){owned_once=1;ui_copy(app->message,sizeof(app->message),"Remote control ready");fprintf(stderr,"hr54-ui: milestone remote-ready uptime-ms=%llu\n",(unsigned long long)now);app->dirty=1;}
        int desired=(app->screen==SCREEN_DOOM||app->doom_running)?0:app->screen==SCREEN_HIDDEN?(app->playback.playing?2:0):app->frontend_prepared?1:0;
        if(input_started&&input.ready&&input.active!=desired){if(input_mode(&input,desired))goto failed;}
        if(app->doom_starting&&input.ready&&!input.active){
            if(app->surface_open){if(hr54_egl_close(&surface))goto failed;app->surface_open=0;}
            app->doom_starting=0;if(api_doom_start(&app->api)){app->screen=SCREEN_HOME;app_error(app,API_FAILED);}
        }
        /* Menu coordinates fill the receiver's output canvas. A fixed 3:2 fit
         * pillarboxes this 720x480 source on HD/anamorphic HDMI output. Doom
         * retains its separate gameplay aspect policy. */
        if(app->ready&&app->screen!=SCREEN_DOOM&&!app->doom_running&&!app->surface_open&&!api_busy(&app->api,API_OPERATION)){if(hr54_egl_open(&surface,&hr54_receiver_egl_api,UI_WIDTH,UI_HEIGHT,HR54_NATIVE_FOREGROUND_DEPTH))goto failed;app->surface_open=1;ui_copy(app->message,sizeof(app->message),"Display ready; waiting for remote");fprintf(stderr,"hr54-ui: milestone display-ready uptime-ms=%llu\n",(unsigned long long)now);app->dirty=1;}
        if(app->surface_open)surface.replace_frame=1;
        /* Requery geometry without submitting idle frames. Submission handles
         * changed geometry with a fitted viewport and the retained texture. */
        static uint64_t geometry_due;if(app->surface_open&&now>=geometry_due){geometry_due=now+1000;int w=0,h=0;if(!surface.api->eglQuerySurface(surface.display,surface.window,0x3057,&w)||!surface.api->eglQuerySurface(surface.display,surface.window,0x3056,&h))goto failed;if(w!=surface.output_width||h!=surface.output_height)app->dirty=1;}
        int hidden=app->screen==SCREEN_HIDDEN||app->screen==SCREEN_DOOM;
        if(app->surface_open&&(app->dirty||app_animating(app,now))&&(!hidden||last_hidden!=hidden)){Screen saved=app->screen;if(app->screen==SCREEN_HOME&&(!owned_once||!app->frontend_prepared))app->screen=SCREEN_STARTUP;int first_home=app->screen==SCREEN_HOME&&!home_submitted;uint64_t render_begin=ui_now();render_screen(&frame,app,now);uint64_t render_end=ui_now();app->screen=saved;if(hr54_egl_submit(&surface,&frame,(UiRect){0,0,UI_WIDTH,UI_HEIGHT}))goto failed;uint64_t present_end=ui_now();if(input.trace&&app->last_key&&render_key_reported!=app->last_key){fprintf(stderr,"render-trace: screen=%d key-ms=%llu controller-to-render-ms=%llu cpu-ms=%llu submit-ms=%llu total-after-key-ms=%llu\n",app->screen,(unsigned long long)app->last_key,(unsigned long long)(render_begin-app->last_key),(unsigned long long)(render_end-render_begin),(unsigned long long)(present_end-render_end),(unsigned long long)(present_end-app->last_key));render_key_reported=app->last_key;}if(first_home){home_submitted=1;fprintf(stderr,"hr54-ui: milestone first-home-frame uptime-ms=%llu\n",(unsigned long long)now);}app->dirty=0;last_hidden=hidden;}
#else
        char chars[64];ssize_t n=read(STDIN_FILENO,chars,sizeof(chars));for(ssize_t i=0;i<n;i++){if(chars[i]=='q'){quit=1;break;}UiKey key=host_key(chars[i]);if(key){controller_key(app,(KeyEvent){key,1,0,0});controller_key(app,(KeyEvent){key,0,0,0});}}
        /* Host lifecycle simulates presentation release, still using API. */
        if(app->doom_starting){app->doom_starting=0;if(api_doom_start(&app->api))app_error(app,API_FAILED);}
        if(app->dirty||app_animating(app,now)){render_screen(&frame,app,now);app->dirty=0;if(output&&snapshot(output,&frame)){result=1;break;}}

#endif
        if(runtime_end&&now>=runtime_end)break;
        struct timespec delay={0,app_animating(app,now)?16000000:20000000};nanosleep(&delay,NULL);
    }
#ifdef HR54_RECEIVER
    input_close(&input);if(app->surface_open&&hr54_egl_close(&surface))result=1;close(lockfd);
#endif
    app_free(app);free(app);ui_fb_free(&frame);return result;
#ifdef HR54_RECEIVER
    failed:fprintf(stderr,"hr54-ui: presentation or input failed; returning ownership\n");input_close(&input);if(app->surface_open)hr54_egl_close(&surface);close(lockfd);app_free(app);free(app);ui_fb_free(&frame);return 1;
#endif
}
