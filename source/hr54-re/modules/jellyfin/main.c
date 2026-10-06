#include "../shared/sdk.h"
#include "../../reboxd/module_auth.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/time.h>
#define JF_NAME_MAX 256
#define JF_ID_MAX 80
#define JF_URL_MAX 2048
#define JF_STREAM_SLOTS 4
#define RELAY_CHUNK 65536
#define RESP_CAP (4*1024*1024)
#define QC_LIFETIME 600
#define CLAIM_WAIT 15
#define POST_STOP_DISMISS 8
static const char *persist_root;
struct stream_slot {int in_use,claimed,ready,closed,paused,draining;char token[64],upath[JF_URL_MAX],session[64];double paused_since,paused_total,duration;};
static struct {
    char jf_host[64],device_id[33],token[512],user_id[80],user_name[256],qc_secret[512],qc_code[16];
    int jf_port,auth_valid,jf_quality_bitrate,playing,play_live,return_to_tv,play_base;
    double qc_started,play_started,play_duration;
    char play_token[64],play_session[64],stop_pending[64],play_item[JF_ID_MAX];
    struct stream_slot streams[JF_STREAM_SLOTS];
} shared,*S=&shared;
static pthread_mutex_t state_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t operation_mutex=PTHREAD_MUTEX_INITIALIZER;
static void state_lock(void){pthread_mutex_lock(&state_mutex);}
static void state_unlock(void){pthread_mutex_unlock(&state_mutex);}
static void jf_log(const char *fmt,...){va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);fputc('\n',stderr);va_end(ap);}
static int random_hex(char *p,size_t n){return rb_random(p,n);}
static int atomic_write(const char *p,const char *b,size_t n,mode_t mode){(void)mode;return rb_atomic(p,b,n);}
static char *read_file(const char *p,size_t *n){return rb_read(p,RESP_CAP,n);}
#include "http.inc"
#include "config.inc"
#include "library.inc"
static struct stream_slot *slot_by_token(const char *token){for(int i=0;i<JF_STREAM_SLOTS;i++)if(S->streams[i].in_use&&!strcmp(token,S->streams[i].token))return &S->streams[i];return NULL;}
static void playback_end_locked(void){if(*S->play_session)strcpy(S->stop_pending,S->play_session);S->play_session[0]=0;S->playing=0;for(int i=0;i<JF_STREAM_SLOTS;i++){S->streams[i].closed=1;S->streams[i].in_use=0;}}
static double playback_elapsed_locked(void){struct stream_slot *s=slot_by_token(S->play_token);return S->play_base+mono_now()-S->play_started-(s?s->paused_total+(s->paused?mono_now()-s->paused_since:0):0);}
static void schedule_tv_return(int a,double b){(void)a;(void)b;/* Presentation belongs to core. */}
static int is_alnum_str(const char *p){if(!p||!*p)return 0;return strspn(p,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")==strlen(p);}
static void send_json_error(int fd,int code,const char *msg,const char *method){(void)method;rb_http_error(fd,code,msg);}
#include "relay.inc"
static int prepare(struct jval *v,struct sb *out) {
    const char *id=jstr(jget(v,"itemId"));double start=jnum(jget(v,"startSeconds"),0);
    if(!is_alnum_str(id)||strlen(id)>=JF_ID_MAX||start<0||start>604800)return fail("invalid item or position");
    if(require_token())return -1;
    char title[256],upath[2048],session[64],token[33];double duration;
    if(transcode_url(id,start,title,sizeof title,upath,sizeof upath,session,sizeof session,&duration)||random_hex(token,32))return -1;
    state_lock();playback_end_locked();struct stream_slot *s=&S->streams[0];memset(s,0,sizeof *s);s->in_use=1;s->duration=duration;
    strcpy(s->token,token);strcpy(s->upath,upath);strcpy(s->session,session);strcpy(S->play_item,id);strcpy(S->play_token,token);strcpy(S->play_session,session);S->play_duration=duration;S->play_started=mono_now();S->play_base=(int)start;S->playing=1;state_unlock();flush_stop_report();
    sb_puts(out,"{\"ok\":true,\"type\":\"stream\",\"title\":");sb_json_str(out,title);sb_fmt(out,",\"live\":false,\"duration\":%.0f,\"position\":%.0f,\"stream\":{\"kind\":\"moduleProxy\",\"token\":\"%s\"},\"session\":\"%s\",\"transport\":{\"stop\":true,\"pause\":true,\"resume\":true,\"seek\":true}}",duration,start,token,token);return 0;
}
static void normalized_item(struct sb *out,struct jval *it,int folder) {
    const char *id=jstr(jget(it,"id")),*name=jstr(jget(it,"name"));sb_puts(out,"{\"id\":");sb_json_str(out,id);sb_puts(out,",\"title\":");sb_json_str(out,name);
    sb_fmt(out,",\"kind\":\"%s\",\"playable\":%s",folder||jbool(jget(it,"isFolder"),0)?"folder":"item",jbool(jget(it,"playable"),0)?"true":"false");sb_puts(out,",\"description\":");sb_json_str(out,jstr(jget(it,"overview")));sb_puts(out,",\"artwork\":");sb_json_str(out,id);
    sb_fmt(out,",\"year\":%.0f,\"duration\":%.0f,\"resume\":%.0f}",jnum(jget(it,"year"),0),jnum(jget(it,"runtime"),0)/10000000,jnum(jget(it,"resumeSeconds"),0));
}
static int resume_items(int offset,struct sb *out){
    if(require_token())return -1;
    struct jval *root=NULL,*rows=NULL;double total=0;
    int rc=items_query("Movie,Episode,Video","IsResumable",1,60,offset,NULL,NULL,&root,&rows,&total);
    if(!rc){sb_fmt(out,"{\"total\":%.0f,\"items\":[",total);for(size_t i=0;jnth(rows,i);i++){if(i)sb_puts(out,",");items_append(out,jnth(rows,i));}sb_puts(out,"]}");}
    jfree(root);return rc;
}
static int browse(const char *query,int search,struct sb *out) {
    char parent[256]="",q[256]="",number[32]="";query_param(query,"parent",parent,sizeof parent);query_param(query,"q",q,sizeof q);query_param(query,"offset",number,sizeof number);int offset=atoi(number);if(offset<0||offset>1000000)return fail("invalid offset");
    struct sb legacy={0},params={0};int rc=0,root=!search&&!*parent;struct jval *v=NULL,*items=NULL;double total=0;
    if(root)rc=jf_libraries(&legacy);
    else if(!strcmp(parent,"@resume"))rc=resume_items(offset,&legacy);
    else{sb_fmt(&params,"offset=%d&limit=60&videoOnly=1&parent=",offset);url_encode(&params,!strcmp(parent,"@all")?"":parent);sb_puts(&params,"&search=");url_encode(&params,search?q:"");rc=jf_items(params.p,&legacy);}
    if(rc)goto done;v=json_parse(legacy.p,legacy.len);items=jget(v,root?"libraries":"items");if(!items||items->t!=J_ARR){rc=fail("invalid library response");goto done;}
    total=root?items->n+2:jnum(jget(v,"total"),items->n);sb_puts(out,"{\"ok\":true,\"items\":[");int emitted=0;
    if(root){const char *ids[]={"@all","@resume"},*names[]={"All titles","Continue Watching"};for(int i=0;i<2;i++)if(i>=offset){if(emitted++)sb_puts(out,",");sb_fmt(out,"{\"id\":\"%s\",\"title\":\"%s\",\"kind\":\"folder\",\"playable\":false}",ids[i],names[i]);}}
    for(size_t i=0;i<items->n&&emitted<60;i++){if(root&&(int)i+2<offset)continue;if(emitted++)sb_puts(out,",");normalized_item(out,items->items[i],root);}
    sb_fmt(out,"],\"total\":%.0f,\"offset\":%d,\"hasMore\":%s}",total,offset,offset+emitted<total?"true":"false");
done:free(legacy.p);free(params.p);jfree(v);return rc;
}
static void settings(struct sb *out){sb_fmt(out,"{\"ok\":true,\"fields\":[{\"key\":\"videoBitrate\",\"label\":\"Picture quality\",\"type\":\"choice\",\"value\":%d,\"choices\":[{\"value\":8000000,\"label\":\"8 Mbps\"},{\"value\":12000000,\"label\":\"12 Mbps\"},{\"value\":16000000,\"label\":\"16 Mbps\"}]}],\"actions\":[{\"id\":\"auth.start\",\"label\":\"Connect\"},{\"id\":\"auth.logout\",\"label\":\"Disconnect\"}]}",S->jf_quality_bitrate);}
static void handle(int fd,const RbRequest *request) {
    char path[2048];strcpy(path,request->path);char *query=strchr(path,'?');if(query)*query++=0;
    if(!strcmp(path,"/status")){state_lock();struct sb out={0};sb_fmt(&out,"{\"ok\":true,\"moduleApi\":1,\"configured\":%s,\"authenticated\":%s,\"playing\":%s,\"playback\":{\"session\":\"%s\",\"playing\":%s}}",*S->jf_host?"true":"false",S->auth_valid?"true":"false",S->playing?"true":"false",S->play_token,S->playing?"true":"false");state_unlock();rb_http_json(fd,200,out.p);free(out.p);return;}
    if(!strncmp(path,"/stream/",8)){serve_play(fd,path+8);return;}
    if(!strcmp(path,"/playback/stop")){
        struct jval *v=json_parse(request->body,request->length);const char *session=jstr(jget(v,"session"));
        state_lock();if(!session||!strcmp(session,S->play_token))playback_end_locked();state_unlock();jfree(v);
        flush_stop_report();rb_http_json(fd,200,"{\"ok\":true}");return;
    }
    pthread_mutex_lock(&operation_mutex);struct sb out={0};struct jval *body=json_parse(*request->body?request->body:"{}",*request->body?request->length:2);int rc=0,code=200;
    if(!body||body->t!=J_OBJ){code=400;goto done;}
    if(!strcmp(path,"/browse")||!strcmp(path,"/search"))rc=browse(query,!strcmp(path,"/search"),&out);
    else if(!strcmp(path,"/legacy/libraries"))rc=jf_libraries(&out);
    else if(!strcmp(path,"/legacy/items"))rc=jf_items(query,&out);
    else if(!strcmp(path,"/legacy/resume")){char offset[16]="";query_param(query,"offset",offset,sizeof offset);rc=resume_items(atoi(offset)<0?0:atoi(offset),&out);}
    else if(!strcmp(path,"/legacy/settings")){
        if(!strcmp(request->method,"POST")){double rate=jnum(jget(body,"jellyfinVideoBitrate"),0);if(body->n!=1||jf_quality_index(rate)<0){code=400;goto done;}state_lock();rc=jf_quality_save_locked(rate);state_unlock();}
        if(!rc){state_lock();jf_settings_locked(&out);state_unlock();}
    }
    else if(!strcmp(path,"/play"))rc=prepare(body,&out);
    else if(!strcmp(path,"/settings")){if(!strcmp(request->method,"POST")){double rate=jnum(jget(body,"videoBitrate"),0);if(body->n!=1||jf_quality_index(rate)<0){code=400;goto done;}state_lock();rc=jf_quality_save_locked(rate);state_unlock();}if(!rc)settings(&out);}
    else if(!strcmp(path,"/actions/auth.start")){rc=qc_start(&out);if(!rc){out.p[--out.len]=0;sb_puts(&out,",\"message\":\"Approve this code in Jellyfin Quick Connect\",\"pollAction\":\"auth.poll\"}");}}
    else if(!strcmp(path,"/actions/auth.poll"))rc=qc_poll(&out);
    else if(!strcmp(path,"/actions/auth.logout"))rc=auth_logout(&out);
    else if(!strcmp(path,"/actions/auth.status")){state_lock();auth_status_locked(&out);state_unlock();}
    else if(!strcmp(path,"/playback/pause")||!strcmp(path,"/playback/resume")||!strcmp(path,"/playback/seek")){
        int seek=!strcmp(path,"/playback/seek"),pause=!strcmp(path,"/playback/pause"),restart=0;
        char item[JF_ID_MAX];double position=0;
        state_lock();struct stream_slot *slot=slot_by_token(S->play_token);
        const char *session=jstr(jget(body,"session"));
        if(!slot||(session&&strcmp(session,S->play_token))||(!seek&&slot->draining))code=409;
        else {
            restart=seek||(!pause&&slot->paused&&mono_now()-slot->paused_since>20);
            position=playback_elapsed_locked();
            if(seek){struct jval *delta=jget(body,"delta");position=jnum(jget(body,"position"),jnum(jget(body,"seconds"),delta?position+jnum(delta,0):-1));if(delta&&position<0)position=0;}
            strcpy(item,S->play_item);
            if(restart&&(position<0||position>604800))code=400;
            else if(!restart){
                if(pause&&!slot->paused)slot->paused_since=mono_now();
                if(!pause&&slot->paused)slot->paused_total+=mono_now()-slot->paused_since;
                slot->paused=pause;sb_fmt(&out,"{\"ok\":true,\"paused\":%s}",pause?"true":"false");
            }
        }
        state_unlock();
        if(restart&&code==200){struct sb params={0};sb_puts(&params,"{\"itemId\":");sb_json_str(&params,item);sb_fmt(&params,",\"startSeconds\":%.3f}",position);struct jval *p=json_parse(params.p,params.len);rc=prepare(p,&out);jfree(p);free(params.p);}
    }
    else if(!strncmp(path,"/art/",5)){const char *id=path+5;if(!is_alnum_str(id)){code=404;goto done;}struct sb url={0};sb_fmt(&url,"Items/%s/Images/Primary?maxWidth=512&maxHeight=512&quality=85",id);struct jf_resp response={0};rc=jf_get(url.p,&response,2*1024*1024);free(url.p);if(!rc&&response.status==200)rb_http_reply(fd,200,!strcmp(response.ctype,"image/png")?"image/png":"image/jpeg",response.body,response.body_len);else rb_http_error(fd,404,"artwork unavailable");resp_free(&response);goto released;}
    else code=404;
done:if(rc)rb_http_error(fd,502,g_err);else if(code!=200)rb_http_error(fd,code,"unsupported or invalid module operation");else rb_http_json(fd,200,out.p?out.p:"{}");
released:jfree(body);free(out.p);pthread_mutex_unlock(&operation_mutex);
}
static void configuration(void){
    char path[1024];size_t n;snprintf(path,sizeof path,"%s/config/config.json",persist_root);char *raw=rb_read(path,16384,&n);struct jval *v=raw?json_parse(raw,n):NULL;free(raw);
    const char *server=jstr(jget(v,"server"));unsigned port=0;char host[64];if(server&&sscanf(server,"http://%63[0-9.]:%u",host,&port)==2&&port>0&&port<=65535){strcpy(S->jf_host,host);S->jf_port=port;}
    const char *id=jstr(jget(v,"user_id")),*name=jstr(jget(v,"user_name"));if(id&&strlen(id)<sizeof S->user_id)strcpy(S->user_id,id);if(name&&strlen(name)<sizeof S->user_name)strcpy(S->user_name,name);jfree(v);
    if(!*S->jf_host){FILE *f=fopen("/var/hr54-persist/rebox.conf","r");if(f){char line[256];while(fgets(line,sizeof line,f)){line[strcspn(line,"\r\n")]=0;if(!strncmp(line,"JELLYFIN_IPV4=",14)&&strlen(line+14)<sizeof S->jf_host)strcpy(S->jf_host,line+14);if(!strncmp(line,"JELLYFIN_PORT=",14))S->jf_port=atoi(line+14);}fclose(f);}}
    load_persist();S->auth_valid=*S->token&&*S->user_id;
}
int main(void){persist_root=getenv("REBOX_MODULE_DATA");if(!persist_root)return 2;
#ifndef REBOX_HOST_TEST
    /* Deliberate compatibility: preserve existing tokens, device ID and quality.
     * This module owns the legacy location; core never interprets it. */
    struct stat legacy;if(!lstat("/var/hr54-persist/jellyfin",&legacy)&&S_ISDIR(legacy.st_mode)&&legacy.st_uid==geteuid())persist_root="/var/hr54-persist/jellyfin";
#endif
const char *dirs[]={"config","state","cache","log"};for(size_t i=0;i<4;i++){char path[1024];snprintf(path,sizeof path,"%s/%s",persist_root,dirs[i]);if(rb_mkdir(path))return 1;}configuration();return rb_module_serve(handle);}
