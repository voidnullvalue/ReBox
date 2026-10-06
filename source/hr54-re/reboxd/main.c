#include "module_registry.h"
#include "http.h"
#include "module_process.h"
#include "module_rpc.h"
#include "playback.h"
static volatile sig_atomic_t stopping;
static ReboxRegistry registry;
static void stop_signal(int sig){(void)sig;stopping=1;}
static void request(int fd) {
    RbRequest *q=calloc(1,sizeof *q);if(!q){rb_http_error(fd,500,"out of memory");return;}
    int code=rb_http_read(fd,q);struct sb out={0};
    if(code)rb_http_error(fd,code,"invalid HTTP request");
    else if(!strcmp(q->method,"GET")&&!strcmp(q->path,"/api/modules")){rb_registry_json(&registry,&out);rb_http_json(fd,200,out.p);}
    else if(!strcmp(q->method,"GET")&&!strcmp(q->path,"/api/state")){rb_playback_json(&out);rb_http_json(fd,200,out.p);}
    else if(!strcmp(q->method,"POST")&&!strncmp(q->path,"/api/playback/",14)){int code=rb_transport(&registry,q->path+14,q->body,&out);if(code==200)rb_http_json(fd,200,out.p);else rb_http_error(fd,code,"transport failed or unsupported");}
    else if(!strncmp(q->path,"/api/modules/",13)){
        char id[64];const char *end=strchr(q->path+13,'/');size_t n=end?(size_t)(end-(q->path+13)):strlen(q->path+13);
        if(n>=sizeof id){rb_http_error(fd,400,"invalid module ID");goto done;}memcpy(id,q->path+13,n);id[n]=0;
        ReboxModule *m=rb_registry_find(&registry,id);
        if(!m)rb_http_error(fd,404,"module not found");
        else if(!end&&!strcmp(q->method,"GET")){rb_module_json(m,&out);rb_http_json(fd,200,out.p);}
        else if(!end)rb_http_error(fd,405,"method not allowed");
        else if(!m->installed)rb_http_error(fd,404,"module not installed");
        else if(!m->enabled||!m->healthy)rb_http_error(fd,409,"module unavailable");
        else if(end&&!strcmp(end,"/play")&&!strcmp(q->method,"POST")){int code=rb_play(&registry,m,q->body,&out);if(code==200)rb_http_json(fd,200,out.p);else rb_http_error(fd,code,"playback preparation failed");}
        else if(end&&!strcmp(q->method,"GET")&&((!strncmp(end,"/browse",7)&&m->browse)||(!strncmp(end,"/search",7)&&m->search))){
            RbReply reply;if(rb_rpc(m,"GET",end,NULL,&reply,120))rb_http_error(fd,502,"module request failed");
            else{rb_http_reply(fd,reply.status,"application/json",reply.body.p,reply.body.len);rb_reply_free(&reply);}
        }else rb_http_error(fd,404,"unsupported module operation");
    } else if(!strcmp(q->method,"GET")&&!strcmp(q->path,"/api/system/status"))rb_http_json(fd,200,"{\"ok\":true,\"ready\":true,\"frontend\":\"native\",\"mediaBusy\":false}");
    else rb_http_error(fd,404,"not found");
done:free(out.p);free(q);
}
int main(int argc,char **argv) {
    const char *root=REBOX_ROOT;int port=8130;
    if(argc>1)root=argv[1];if(argc>2)port=atoi(argv[2]);if(argc>4||port<1||port>65535)return 2;
    umask(077);if(rb_registry_load(&registry,root)){fprintf(stderr,"reboxd: %s\n",g_err);return 1;}
    if(rb_process_init(argc>3?argv[3]:"/tmp/rebox-modules"))return 1;
    for(size_t i=0;i<registry.count;i++)if(registry.modules[i].enabled)rb_process_start(&registry,&registry.modules[i],1);
    int listener=rb_listen_tcp(port);if(listener<0){perror("reboxd listener");return 1;}
    signal(SIGPIPE,SIG_IGN);signal(SIGTERM,stop_signal);signal(SIGINT,stop_signal);
    while(!stopping){rb_process_tick(&registry);struct pollfd p={listener,POLLIN,0};if(poll(&p,1,200)<=0)continue;int fd=accept4(listener,NULL,NULL,SOCK_CLOEXEC);if(fd>=0){request(fd);close(fd);}}
    close(listener);struct sb ignored={0};if(rb_playback_busy(NULL))rb_transport(&registry,"stop","{}",&ignored);free(ignored.p);rb_process_shutdown(&registry);return 0;
}
