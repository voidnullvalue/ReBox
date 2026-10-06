#include "module_registry.h"
#include "http.h"
static volatile sig_atomic_t stopping;
static ReboxRegistry registry;
static void stop_signal(int sig){(void)sig;stopping=1;}
static void request(int fd) {
    RbRequest *q=calloc(1,sizeof *q);if(!q){rb_http_error(fd,500,"out of memory");return;}
    int code=rb_http_read(fd,q);struct sb out={0};
    if(code)rb_http_error(fd,code,"invalid HTTP request");
    else if(!strcmp(q->method,"GET")&&!strcmp(q->path,"/api/modules")){rb_registry_json(&registry,&out);rb_http_json(fd,200,out.p);}
    else if(!strcmp(q->method,"GET")&&!strncmp(q->path,"/api/modules/",13)){
        ReboxModule *m=rb_registry_find(&registry,q->path+13);if(m){rb_module_json(m,&out);rb_http_json(fd,200,out.p);}else rb_http_error(fd,404,"module not found");
    } else if(!strcmp(q->method,"GET")&&!strcmp(q->path,"/api/system/status"))rb_http_json(fd,200,"{\"ok\":true,\"ready\":true,\"frontend\":\"native\",\"mediaBusy\":false}");
    else rb_http_error(fd,404,"not found");
    free(out.p);free(q);
}
int main(int argc,char **argv) {
    const char *root=REBOX_ROOT;int port=8130;
    if(argc>1)root=argv[1];if(argc>2)port=atoi(argv[2]);if(argc>3||port<1||port>65535)return 2;
    umask(077);if(rb_registry_load(&registry,root)){fprintf(stderr,"reboxd: %s\n",g_err);return 1;}
    int listener=rb_listen_tcp(port);if(listener<0){perror("reboxd listener");return 1;}
    signal(SIGPIPE,SIG_IGN);signal(SIGTERM,stop_signal);signal(SIGINT,stop_signal);
    while(!stopping){struct pollfd p={listener,POLLIN,0};if(poll(&p,1,200)<=0)continue;int fd=accept4(listener,NULL,NULL,SOCK_CLOEXEC);if(fd>=0){request(fd);close(fd);}}
    close(listener);return 0;
}
