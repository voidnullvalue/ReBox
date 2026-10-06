#include "sdk.h"
static volatile sig_atomic_t quit;
static pthread_mutex_t count_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned clients;
static RbModuleHandler dispatch;
static void stop(int sig){(void)sig;quit=1;}
static void *request(void *arg){int fd=(int)(intptr_t)arg;RbRequest *r=calloc(1,sizeof *r);
    if(r){int rc=rb_http_read(fd,r);if(rc)rb_http_error(fd,rc,"invalid request");else dispatch(fd,r);free(r);}
    close(fd);pthread_mutex_lock(&count_lock);clients--;pthread_mutex_unlock(&count_lock);return NULL;
}
int rb_module_serve(RbModuleHandler handler) {
    const char *socket=getenv("REBOX_MODULE_SOCKET"),*api=getenv("REBOX_MODULE_API");if(!socket||!api||strcmp(api,"1"))return 2;umask(077);dispatch=handler;
    int listener=rb_listen_unix(socket);if(listener<0)return 1;signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);
    pid_t owner=getppid();
    while(!quit&&getppid()==owner){struct pollfd p={listener,POLLIN,0};if(poll(&p,1,100)<=0)continue;int fd=accept4(listener,NULL,NULL,SOCK_CLOEXEC);if(fd<0)continue;
        pthread_mutex_lock(&count_lock);if(clients>=8){pthread_mutex_unlock(&count_lock);close(fd);continue;}clients++;pthread_mutex_unlock(&count_lock);
        pthread_t worker;if(pthread_create(&worker,NULL,request,(void *)(intptr_t)fd)){close(fd);pthread_mutex_lock(&count_lock);clients--;pthread_mutex_unlock(&count_lock);}else pthread_detach(worker);
    }
    close(listener);unlink(socket);return 0;
}
