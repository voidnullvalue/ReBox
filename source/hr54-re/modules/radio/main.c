/* Radio owns playlists, hierarchy, metadata, upstream probing and relay.
 * Core alone starts/stops the receiver decoder from our inert stream plan. */
#include "../shared/sdk.h"
#include "../../reboxd/module_auth.h"
#include <ctype.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#define REBOX_RADIO_MODULE 1
static const char *persist_root;
static char package_root[1024];
static char surf_anchor[32],surf_cursor[32];
static struct {
    int iptv_active,iptv_claimed,return_to_tv;
    pid_t iptv_worker;
    char iptv_token[33],iptv_url[8193],iptv_ua[2049],iptv_ref[2049],iptv_error[256];
} shared,*S=&shared;
static pthread_mutex_t state_mutex=PTHREAD_MUTEX_INITIALIZER,operation_mutex=PTHREAD_MUTEX_INITIALIZER;
static void state_lock(void){pthread_mutex_lock(&state_mutex);}
static void state_unlock(void){pthread_mutex_unlock(&state_mutex);}
static void jf_log(const char *fmt,...){va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);fputc('\n',stderr);va_end(ap);}
static char *read_file(const char *p,size_t *n){return rb_read(p,4096,n);}
static int atomic_write(const char *p,const char *b,size_t n,mode_t mode){(void)mode;return rb_atomic(p,b,n);}
static void playback_end_locked(void){S->iptv_active=S->iptv_claimed=0;S->iptv_worker=0;}
static void send_json_error(int fd,int code,const char *msg,const char *method){(void)method;rb_http_error(fd,code,msg);}
#include "library.inc"
#include "radio-stream.inc"


/* Group IDs are bounded opaque cursors, independent of potentially long labels. */
static void group_id(char *out,size_t n,const char *name){snprintf(out,n,"group-%016llx",(unsigned long long)iptv_hash(UINT64_C(14695981039346656037),name));}
static int page_number(const char *query,const char *key,int fallback,int maximum){char n[32]="";if(!query_param(query,key,n,sizeof n))return fallback;char *end;long v=strtol(n,&end,10);return !*n||*end||v<0||v>maximum?-1:(int)v;}
static void folder(struct sb *out,const char *id,const char *title){sb_puts(out,"{\"id\":");sb_json_str(out,id);sb_puts(out,",\"title\":");sb_json_str(out,title);sb_puts(out,",\"kind\":\"folder\",\"playable\":false}");}
static void channel(struct sb *out,const struct iptv_channel *c){sb_puts(out,"{\"id\":");sb_json_str(out,c->id);sb_puts(out,",\"title\":");sb_json_str(out,c->name);sb_puts(out,",\"subtitle\":");sb_json_str(out,*c->group?c->group:c->tvg);sb_puts(out,",\"kind\":\"item\",\"playable\":true,\"artwork\":");sb_json_str(out,*c->logo?c->id:"");sb_puts(out,"}");}
static int browse(const char *query,int search,struct sb *out){
    if(!iptv_loaded)return fail("%s",iptv_library_error);
    char parent[256]="",q[256]="";query_param(query,"parent",parent,sizeof parent);query_param(query,"q",q,sizeof q);
    int offset=page_number(query,"offset",0,IPTV_MAX_CHANNELS+1),limit=page_number(query,"limit",60,60);if(offset<0||limit<1)return fail("invalid page");
    const char *group=NULL;
    if(!search&&*parent&&strcmp(parent,"all")){for(size_t i=0;i<iptv_count;i++){char id[32];group_id(id,sizeof id,iptv_channels[i].group);if(!strcmp(id,parent)){group=iptv_channels[i].group;break;}}if(!group)return fail("unknown folder");}
    int emitted=0,total=0;sb_puts(out,"{\"ok\":true,\"items\":[");
    if(!search&&!*parent){
        total=1;if(offset==0){folder(out,"all","All stations");emitted++;}
        for(size_t i=0;i<iptv_count;i++){const char *g=iptv_channels[i].group;int seen=0;for(size_t j=0;j<i;j++)if(!strcmp(g,iptv_channels[j].group)){seen=1;break;}if(seen)continue;int at=total++;if(at<offset||emitted>=limit)continue;char id[32];group_id(id,sizeof id,g);if(emitted++)sb_puts(out,",");folder(out,id,*g?g:"Other channels");}
    }else for(size_t i=0;i<iptv_count;i++){struct iptv_channel *c=&iptv_channels[i];if(group&&strcmp(group,c->group))continue;if(search&&*q&&!iptv_contains(c->name,q)&&!iptv_contains(c->tvg,q)&&!iptv_contains(c->group,q))continue;int at=total++;if(at<offset||emitted>=limit)continue;if(emitted++)sb_puts(out,",");channel(out,c);}
    sb_fmt(out,"],\"total\":%d,\"offset\":%d,\"hasMore\":%s}",total,offset,offset+emitted<total?"true":"false");return 0;
}
static int prepare(struct jval *body,struct sb *out){
    const char *id=jstr(jget(body,"itemId"));if(!id)id=jstr(jget(body,"channelId"));struct iptv_channel *c=iptv_find(id);if(!c)return fail("Unknown Radio channel ID");
    surf_anchor[0]=surf_cursor[0]=0;
    char token[33];if(rb_random(token,32))return -1;
    state_lock();playback_end_locked();strcpy(S->iptv_token,token);S->iptv_active=1;S->iptv_error[0]=0;snprintf(S->iptv_url,sizeof S->iptv_url,"%s",c->url);state_unlock();
    sb_puts(out,"{\"ok\":true,\"type\":\"stream\",\"title\":");sb_json_str(out,c->name);sb_fmt(out,",\"live\":true,\"duration\":0,\"stream\":{\"kind\":\"moduleProxy\",\"token\":\"%s.ts\"},\"session\":\"%s\",\"transport\":{\"stop\":true,\"pause\":false,\"resume\":false,\"seek\":false,\"channelUp\":true,\"channelDown\":true}}",token,token);return 0;
}
static void status(struct sb *out){
    /* Cached library health is independent of current upstream availability. */
    state_lock();sb_fmt(out,"{\"ok\":true,\"moduleApi\":1,\"active\":%s,\"workerPid\":%d,\"playback\":{\"session\":\"%s\",\"playing\":%s},\"error\":",S->iptv_active?"true":"false",S->iptv_worker,S->iptv_token,S->iptv_active?"true":"false");sb_json_str(out,S->iptv_error);sb_puts(out,"}");state_unlock();
}
static int adjacent(struct jval *body,struct sb *out){
 const char *id=jstr(jget(body,"itemId"));double direction=jnum(jget(body,"direction"),0);if(direction!=1&&direction!=-1)return fail("invalid direction");
 size_t at=iptv_count;for(size_t i=0;i<iptv_count;i++)if(id&&!strcmp(id,iptv_channels[i].id)){at=i;break;}if(at==iptv_count)return fail("channel no longer in playlist");
 if(!strcmp(surf_anchor,id)&&*surf_cursor){for(size_t i=0;i<iptv_count;i++)if(!strcmp(surf_cursor,iptv_channels[i].id)){at=i;break;}}
 size_t next=direction>0?(at+1)%iptv_count:(at+iptv_count-1)%iptv_count;
 snprintf(surf_anchor,sizeof surf_anchor,"%s",id);snprintf(surf_cursor,sizeof surf_cursor,"%s",iptv_channels[next].id);
 char token[33];state_lock();snprintf(token,sizeof token,"%s",S->iptv_token);state_unlock();if(radio_probe(iptv_channels[next].url,token))return -1;
 
 sb_puts(out,"{\"ok\":true,\"itemId\":");sb_json_str(out,iptv_channels[next].id);sb_puts(out,"}");return 0;
}
static void handle(int fd,const RbRequest *request){
    char path[2048];strcpy(path,request->path);char *query=strchr(path,'?');if(query)*query++=0;
    if(!strcmp(path,"/status")){struct sb out={0};status(&out);rb_http_json(fd,200,out.p);free(out.p);return;}
    if(!strncmp(path,"/stream/",8)){iptv_stream(fd,path+8,request->method);return;}
    if(!strcmp(path,"/playback/stop")){struct jval *v=json_parse(request->body,request->length);const char *session=jstr(jget(v,"session"));state_lock();if(!session||!strcmp(session,S->iptv_token))playback_end_locked();state_unlock();jfree(v);rb_http_json(fd,200,"{\"ok\":true}");return;}
    pthread_mutex_lock(&operation_mutex);iptv_reload();struct sb out={0};struct jval *body=json_parse(*request->body?request->body:"{}",*request->body?request->length:2);int rc=0,code=200;
    if(!body||body->t!=J_OBJ){code=400;goto done;}
    if(!strcmp(path,"/browse")||!strcmp(path,"/search"))rc=browse(query,!strcmp(path,"/search"),&out);
    else if(!strcmp(path,"/adjacent"))rc=adjacent(body,&out);
    else if(!strcmp(path,"/play"))rc=prepare(body,&out);
    else if(!strcmp(path,"/legacy/groups")||!strcmp(path,"/legacy/channels"))rc=iptv_library_api(!strcmp(path,"/legacy/groups")?"/api/iptv/groups":"/api/iptv/channels",query,&out);
    else if(!strcmp(path,"/settings"))sb_puts(&out,"{\"ok\":true,\"fields\":[],\"actions\":[{\"id\":\"playlist.reload\",\"label\":\"Reload playlist\"}]}");
    else if(!strcmp(path,"/actions/playlist.reload")){iptv_loaded=0;iptv_reload();if(!iptv_loaded)rc=fail("%s",iptv_library_error);else sb_fmt(&out,"{\"ok\":true,\"message\":\"Loaded %zu channels\"}",iptv_count);}

    else code=404;
done:if(rc)rb_http_error(fd,502,g_err);else if(code!=200)rb_http_error(fd,code,"invalid module operation");else rb_http_json(fd,200,out.p?out.p:"{}");
jfree(body);free(out.p);pthread_mutex_unlock(&operation_mutex);
}

int main(int argc,char **argv){(void)argc;persist_root=getenv("REBOX_MODULE_DATA");if(!persist_root)return 2;

    char executable[1024];if(!realpath(argv[0],executable))return 1;char *slash=strrchr(executable,'/');if(!slash)return 1;*slash=0;slash=strrchr(executable,'/');if(!slash)return 1;*slash=0;snprintf(package_root,sizeof package_root,"%s",executable);
    if(rb_module_seed("default-data",""))return 1;
    if(getenv("REBOX_MODULE_SEED_ONLY"))return 0;
    const char *dirs[]={"radio","state"};for(size_t i=0;i<2;i++){char p[1024];snprintf(p,sizeof p,"%s/%s",persist_root,dirs[i]);if(rb_mkdir(p))return 1;}iptv_reload();

    return rb_module_serve(handle);
}
