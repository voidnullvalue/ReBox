#include "../shared/sdk.h"
#include "../../reboxd/module_auth.h"
#include <ctype.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#define REBOX_YOUTUBE_MODULE 1
#define JF_ID_MAX 80
static const char *persist_root;
static int automatic_updates=1;
static struct {
    int playing,play_live,yt_active,yt_claimed;
    pid_t yt_updater;double yt_update_check,yt_duration,yt_claim_at,play_started;
    char yt_token[33],yt_video[8193],yt_audio[8193],yt_error[256],play_item[JF_ID_MAX];
} shared,*S=&shared;
static pthread_mutex_t state_mutex=PTHREAD_MUTEX_INITIALIZER,operation_mutex=PTHREAD_MUTEX_INITIALIZER;
static void state_lock(void){pthread_mutex_lock(&state_mutex);}
static void state_unlock(void){pthread_mutex_unlock(&state_mutex);}
static int random_hex(char *p,size_t n){return rb_random(p,n);}
static char *read_file(const char *p,size_t *n){return rb_read(p,4096,n);}
static int atomic_write(const char *p,const char *b,size_t n,mode_t mode){(void)mode;return rb_atomic(p,b,n);}
static void playback_end_locked(void){S->playing=S->yt_active=S->yt_claimed=0;}
static void send_json_error(int fd,int code,const char *msg,const char *method){(void)method;rb_http_error(fd,code,msg);}
#include "runtime.inc"
static int claim(char token[33],int playback){
    if(rb_random(token,32))return -1;state_lock();if(S->yt_active){state_unlock();return fail("YouTube operation already active");}
    strcpy(S->yt_token,token);S->yt_active=1;S->yt_claimed=0;S->yt_claim_at=0;S->yt_error[0]=0;S->playing=playback;S->play_live=3;S->play_started=mono_now();state_unlock();return 0;
}
static int prepare(struct jval *body,struct sb *out){
    const char *id=jstr(jget(body,"itemId"));if(!yt_valid_id(id))return fail("Invalid video ID");char token[33];if(claim(token,1))return -1;
    struct sb result={0};int rc=yt_resolve("play",id,"0",token,&result);struct jval *v=rc?NULL:json_parse(result.p,result.len);
    const char *video=jstr(jget(v,"videoUrl")),*audio=jstr(jget(v,"audioUrl")),*title=jstr(jget(v,"title"));double duration=jnum(jget(v,"duration"),0);
    if(!rc&&(!video||!audio||strlen(video)>8192||strlen(audio)>8192||strncmp(video,"https://",8)||strncmp(audio,"https://",8)||strpbrk(video,"\r\n")||strpbrk(audio,"\r\n")||duration<0||duration>604800))rc=fail("Resolver returned invalid media URLs or duration");
    if(!rc){state_lock();if(!S->yt_active||strcmp(S->yt_token,token))rc=fail("YouTube cancelled");else{strcpy(S->yt_video,video);strcpy(S->yt_audio,audio);strcpy(S->play_item,id);S->yt_duration=duration;}state_unlock();}
    if(!rc){sb_puts(out,"{\"ok\":true,\"type\":\"stream\",\"title\":");char bounded[256];snprintf(bounded,sizeof bounded,"%s",title?title:id);sb_json_str(out,bounded);sb_fmt(out,",\"live\":false,\"duration\":%.0f,\"stream\":{\"kind\":\"moduleProxy\",\"token\":\"%s.ts\"},\"session\":\"%s\",\"transport\":{\"stop\":true,\"pause\":false,\"resume\":false,\"seek\":false}}",duration,token,token);}
    else{state_lock();if(!strcmp(S->yt_token,token)){snprintf(S->yt_error,sizeof S->yt_error,"%s",g_err);playback_end_locked();}state_unlock();}
    jfree(v);free(result.p);return rc;
}
static int query_int(const char *query,const char *key,int fallback,int max){char value[32]="";if(!query_param(query,key,value,sizeof value))return fallback;char *end;long n=strtol(value,&end,10);return !*value||*end||n<0||n>max?-1:(int)n;}
static int search(const char *query,int legacy,struct sb *out){
    char q[65]="",number[16]="",token[33];query_param(query,"q",q,sizeof q);int offset=query_int(query,legacy?"page":"offset",0,legacy?100:600),page=legacy?offset:offset/6,limit=query_int(query,"limit",60,60);
    if(!*q||offset<0||page>100||limit<1)return fail("Invalid search");snprintf(number,sizeof number,"%d",page);if(claim(token,0))return -1;
    struct sb result={0};int rc=yt_resolve("search",q,number,token,&result);state_lock();if(!strcmp(S->yt_token,token))S->yt_active=0;state_unlock();
    struct jval *v=rc?NULL:json_parse(result.p,result.len),*rows=jget(v,"results");if(!rc&&(!rows||rows->t!=J_ARR||rows->n>6))rc=fail("invalid resolver results");
    if(!rc&&legacy)sb_putn(out,result.p,result.len);
    else if(!rc){sb_puts(out,"{\"ok\":true,\"items\":[");int count=0;
        for(size_t i=(size_t)(offset%6);i<rows->n&&count<limit;i++){struct jval *it=rows->items[i];const char *id=jstr(jget(it,"id"));if(!yt_valid_id(id))continue;if(count++)sb_puts(out,",");sb_puts(out,"{\"id\":");sb_json_str(out,id);sb_puts(out,",\"title\":");sb_json_str(out,jstr(jget(it,"title")));sb_puts(out,",\"subtitle\":");sb_json_str(out,jstr(jget(it,"channel")));sb_fmt(out,",\"kind\":\"item\",\"playable\":true,\"duration\":%.0f}",jnum(jget(it,"duration"),0));}
        int more=count&&((size_t)(offset%6+count)<rows->n||jbool(jget(v,"hasMore"),0));sb_fmt(out,"],\"total\":%d,\"offset\":%d,\"hasMore\":%s}",offset+count+more,offset,more?"true":"false");}
    jfree(v);free(result.p);return rc;
}
static void handle(int fd,const RbRequest *request){
    char path[2048];strcpy(path,request->path);char *query=strchr(path,'?');if(query)*query++=0;
    if(!strcmp(path,"/status")){state_lock();struct sb out={0};sb_fmt(&out,"{\"ok\":true,\"moduleApi\":1,\"playback\":{\"session\":\"%s\",\"playing\":%s},\"active\":%s}",S->yt_token,S->playing?"true":"false",S->yt_active?"true":"false");state_unlock();rb_http_json(fd,200,out.p);free(out.p);return;}
    if(!strncmp(path,"/stream/",8)){yt_stream(fd,path+8,request->method);return;}
    if(!strcmp(path,"/playback/stop")){struct jval *v=json_parse(request->body,request->length);const char *session=jstr(jget(v,"session"));state_lock();if(!session||!strcmp(session,S->yt_token))playback_end_locked();state_unlock();jfree(v);rb_http_json(fd,200,"{\"ok\":true}");return;}
    pthread_mutex_lock(&operation_mutex);struct sb out={0};struct jval *body=json_parse(*request->body?request->body:"{}",*request->body?request->length:2);int rc=0,code=200;
    if(!body||body->t!=J_OBJ)code=400;
    else if(!strcmp(path,"/search")||!strcmp(path,"/legacy/search"))rc=search(query,!strcmp(path,"/legacy/search"),&out);
    else if(!strcmp(path,"/play"))rc=prepare(body,&out);
    else if(!strcmp(path,"/legacy/state"))rc=yt_state(!strcmp(request->method,"POST")?body:NULL,&out);
    else if(!strcmp(path,"/legacy/status"))rc=yt_status(&out);
    else if(!strcmp(path,"/settings")){
        if(!strcmp(request->method,"POST")){struct jval *value=jget(body,"automaticUpdates");if(body->n!=1||!value||(value->t!=J_TRUE&&value->t!=J_FALSE))code=400;
            else{char file[1024];snprintf(file,sizeof file,"%s/state/youtube-settings.json",persist_root);rc=rb_atomic(file,request->body,request->length);if(!rc)automatic_updates=jbool(value,1);}}
        if(!rc&&code==200)sb_fmt(&out,"{\"ok\":true,\"fields\":[{\"key\":\"automaticUpdates\",\"label\":\"Automatic runtime updates\",\"type\":\"bool\",\"value\":%s}],\"actions\":[]}",automatic_updates?"true":"false");
    }
    else code=404;
    if(rc)rb_http_error(fd,502,g_err);else if(code!=200)rb_http_error(fd,code,"invalid module operation");else rb_http_json(fd,200,out.p?out.p:"{}");jfree(body);free(out.p);pthread_mutex_unlock(&operation_mutex);
}
static void *updater(void *unused){(void)unused;for(;;){pthread_mutex_lock(&operation_mutex);if(automatic_updates)yt_maybe_update();pthread_mutex_unlock(&operation_mutex);nap(1);}return NULL;}
int main(void){persist_root=getenv("REBOX_MODULE_DATA");if(!persist_root)return 2;
    const char *dirs[]={"youtube","state"};for(size_t i=0;i<2;i++){char p[1024];snprintf(p,sizeof p,"%s/%s",persist_root,dirs[i]);if(rb_mkdir(p))return 1;}
#ifndef REBOX_HOST_TEST
    /* Copy current private runtime/auth once; never move or replace legacy data. */
    const char *keep[]={"youtube/bin","youtube/python","youtube/yt-dlp.zip","youtube/version","youtube/cookies.txt","youtube/update-status.json","youtube/yt-dlp.conf","state/youtube-state.json","state/youtube-settings.json"};
    for(size_t i=0;i<sizeof keep/sizeof keep[0];i++){char from[1024],to[1024];snprintf(from,sizeof from,"/var/hr54-persist/jellyfin/%s",keep[i]);snprintf(to,sizeof to,"%s/%s",persist_root,keep[i]);if(rb_module_copy_defaults(from,to))return 1;}
#endif
    if(rb_module_seed("default-data",""))return 1;
    if(getenv("REBOX_MODULE_SEED_ONLY"))return 0;
    char config[1024];size_t n;snprintf(config,sizeof config,"%s/state/youtube-settings.json",persist_root);char *raw=rb_read(config,4096,&n);struct jval *v=raw?json_parse(raw,n):NULL;automatic_updates=jbool(jget(v,"automaticUpdates"),1);jfree(v);free(raw);
    pthread_t updates;if(!pthread_create(&updates,NULL,updater,NULL))pthread_detach(updates);
    return rb_module_serve(handle);
}
