#include "playback.h"
#include "module_rpc.h"
static struct {
    char module[64],item[256],title[256],session[256],token[128];
    int playing,preparing,paused,live,stop,pause,resume,seek;
    unsigned generation;double started,duration;
} active;
int rb_playback_busy(const char *id){return (active.playing||active.preparing)&&(!id||!strcmp(id,active.module));}
static int safe_string(struct jval *v,char *out,size_t cap) {
    const char *s=jstr(v);if(!s||strlen(s)>=cap||strpbrk(s,"\r\n"))return -1;strcpy(out,s);return 0;
}
int rb_transport(ReboxRegistry *r,const char *operation,const char *body,struct sb *out) {
    if(!active.playing&&!active.preparing)return 409;
    int supported=(!strcmp(operation,"stop")&&active.stop)||(!strcmp(operation,"pause")&&active.pause)||(!strcmp(operation,"resume")&&active.resume)||(!strcmp(operation,"seek")&&active.seek);
    if(!supported)return 409;
    ReboxModule *m=rb_registry_find(r,active.module);if(!m)return 409;
    char path[64];snprintf(path,sizeof path,"/playback/%s",operation);RbReply reply;
    if(rb_rpc(m,"POST",path,body,&reply,120))return 502;
    int code=reply.status;rb_reply_free(&reply);if(code!=200)return code;
    if(!strcmp(operation,"stop")){active.playing=active.preparing=0;active.module[0]=0;}
    else if(!strcmp(operation,"pause"))active.paused=1;
    else if(!strcmp(operation,"resume"))active.paused=0;
    sb_puts(out,"{\"ok\":true}");return 200;
}
int rb_play(ReboxRegistry *r,ReboxModule *m,const char *body,struct sb *out) {
    if(!m->installed)return 404;if(!m->enabled||!m->healthy||!m->playback)return 409;
    struct jval *request=json_parse(body,strlen(body));char item[256];
    if(safe_string(jget(request,"itemId"),item,sizeof item)||!*item){jfree(request);return 400;}jfree(request);
    if(rb_playback_busy(NULL)){struct sb stopped={0};int code=rb_transport(r,"stop","{}",&stopped);free(stopped.p);if(code!=200)return code;}
    active.preparing=1;strcpy(active.module,m->id);active.stop=1;
    RbReply reply;int code=502;
    if(rb_rpc(m,"POST","/play",body,&reply,120))goto failed;
    struct jval *plan=json_parse(reply.body.p,reply.body.len);struct jval *stream=jget(plan,"stream"),*transport=jget(plan,"transport");
    const char *type=jstr(jget(plan,"type")),*kind=jstr(jget(stream,"kind"));
    char url[2048];
    if(reply.status!=200||!type||strcmp(type,"stream")||!kind||strcmp(kind,"moduleProxy")||
       safe_string(jget(stream,"token"),active.token,sizeof active.token)||!rb_relative(active.token)||strchr(active.token,'/')||
       safe_string(jget(plan,"session"),active.session,sizeof active.session)||safe_string(jget(plan,"title"),active.title,sizeof active.title)){
        jfree(plan);rb_reply_free(&reply);goto failed;
    }
    active.stop=jbool(jget(transport,"stop"),0);active.pause=jbool(jget(transport,"pause"),0);active.resume=jbool(jget(transport,"resume"),0);active.seek=jbool(jget(transport,"seek"),0);
    active.duration=jnum(jget(plan,"duration"),0);active.live=jbool(jget(plan,"live"),0);
    jfree(plan);rb_reply_free(&reply);
    snprintf(url,sizeof url,"http://127.0.0.1:8130/module-stream/%s/%s",m->id,active.token);
#ifdef REBOX_HOST_TEST
    (void)url;
#else
    /* Fixed executable and argv: module fields never become shell commands. */
    pid_t pid=fork();if(!pid){char *argv[]={"/var/opt/hr54/bin/hr54-play-url",url,NULL};char *env[]={"PATH=/bin:/usr/bin","HR54_NATIVE_FRONTEND=1",NULL};execve(argv[0],argv,env);_exit(127);}
    int status=0;if(pid<0)goto failed;while(waitpid(pid,&status,0)<0&&errno==EINTR){}if(!WIFEXITED(status)||WEXITSTATUS(status))goto failed;
#endif
    strcpy(active.item,item);active.preparing=0;active.playing=1;active.paused=0;active.started=mono_now();active.generation++;rb_playback_json(out);return 200;
failed: {RbReply cleanup;if(!rb_rpc(m,"POST","/playback/stop","{}",&cleanup,2))rb_reply_free(&cleanup);active.preparing=active.playing=0;active.module[0]=0;}return code;
}
void rb_playback_json(struct sb *out) {
    sb_fmt(out,"{\"ok\":true,\"playing\":%s,\"preparing\":%s,\"paused\":%s,\"live\":%s,\"generation\":%u,\"source\":",active.playing?"true":"false",active.preparing?"true":"false",active.paused?"true":"false",active.live?"true":"false",active.generation);
    sb_json_str(out,active.module);sb_puts(out,",\"itemId\":");sb_json_str(out,active.playing?active.item:"");sb_puts(out,",\"title\":");sb_json_str(out,active.playing?active.title:"");
    sb_fmt(out,",\"elapsed\":%.0f,\"duration\":%.0f,\"transport\":{\"stop\":%s,\"pause\":%s,\"resume\":%s,\"seek\":%s}}",active.playing?mono_now()-active.started:0,active.playing?active.duration:0,active.playing&&active.stop?"true":"false",active.playing&&active.pause?"true":"false",active.playing&&active.resume?"true":"false",active.playing&&active.seek?"true":"false");
}
