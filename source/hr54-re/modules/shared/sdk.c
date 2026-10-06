#include "sdk.h"
static volatile sig_atomic_t quit;
static void stop(int sig){(void)sig;quit=1;}
int rb_module_serve(RbModuleHandler handler) {
    const char *socket=getenv("REBOX_MODULE_SOCKET"),*api=getenv("REBOX_MODULE_API");
    if(!socket||!api||strcmp(api,"1"))return 2;umask(077);
    int listener=rb_listen_unix(socket);if(listener<0)return 1;
    signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);
    while(!quit){struct pollfd p={listener,POLLIN,0};if(poll(&p,1,100)<=0)continue;int fd=accept4(listener,NULL,NULL,SOCK_CLOEXEC);if(fd<0)continue;
        RbRequest *r=calloc(1,sizeof *r);if(!r){close(fd);continue;}int rc=rb_http_read(fd,r);
        if(rc)rb_http_error(fd,rc,"invalid request");
        else if(!strcmp(r->method,"GET")&&!strcmp(r->path,"/status"))rb_http_json(fd,200,"{\"ok\":true,\"moduleApi\":1}");
        else handler(fd,r);free(r);close(fd);
    }
    close(listener);unlink(socket);return 0;
}
