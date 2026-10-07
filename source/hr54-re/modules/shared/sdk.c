#include "sdk.h"
void rb_media_origin(const char *stage,const char *url){
    const char *scheme=url?strstr(url,"://"):NULL;
    if(!scheme||(strncmp(url,"http://",7)&&strncmp(url,"https://",8))){fprintf(stderr,"%s URL=[redacted]\n",stage);return;}
    const char *host=scheme+3;size_t n=strcspn(host,"/?#");
    const char *at=memchr(host,'@',n);if(at){n-=(size_t)(at+1-host);host=at+1;}
    if(n>255||strspn(host,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-:[]")<n){fprintf(stderr,"%s URL=[redacted]\n",stage);return;}
    fprintf(stderr,"%s URL=%.*s://%.*s/[redacted]\n",stage,(int)(scheme-url),url,(int)n,host);
}
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

/* Modules opt into data defaults; core never interprets provider data. Existing
 * regular files are retained. Every new file is fsynced and linked atomically,
 * with no replacement and no symlink traversal. Startup can resume after exit. */
#include <dirent.h>
static int seed_tree(const char *source,const char *destination,unsigned *entries,size_t *total,unsigned depth){
    if(depth>16)return fail("module defaults exceed directory depth");
    struct stat st;if(lstat(source,&st))return errno==ENOENT?0:fail("module defaults unavailable");
    if(S_ISLNK(st.st_mode))return fail("module defaults contain a link");
    if(++*entries>4096)return fail("module defaults exceed entry limit");
    if(S_ISDIR(st.st_mode)){
        if(rb_mkdir(destination))return -1;DIR *d=opendir(source);if(!d)return -1;struct dirent *e;int rc=0;
        while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
            char from[REBOX_PATH_MAX],to[REBOX_PATH_MAX];if(rb_path(from,sizeof from,source,e->d_name,NULL)||rb_path(to,sizeof to,destination,e->d_name,NULL)||seed_tree(from,to,entries,total,depth+1)){rc=-1;break;}}
        closedir(d);return rc;
    }
    if(!S_ISREG(st.st_mode)||st.st_size<0||(size_t)st.st_size>128u*1024*1024-*total)return fail("invalid or oversized module defaults");
    *total+=(size_t)st.st_size;
    struct stat present;if(!lstat(destination,&present))return S_ISREG(present.st_mode)?0:fail("module data destination unsafe");if(errno!=ENOENT)return -1;
    char temp[REBOX_PATH_MAX];if(snprintf(temp,sizeof temp,"%s.seed-XXXXXX",destination)>=(int)sizeof temp)return -1;
    int in=open(source,O_RDONLY|O_NOFOLLOW|O_CLOEXEC),out=mkstemp(temp);if(in<0||out<0){if(in>=0)close(in);if(out>=0){close(out);unlink(temp);}return -1;}
    int rc=fchmod(out,(st.st_mode&0111)?0700:0600);size_t copied=0;char bytes[32768];
    while(!rc){ssize_t n=read(in,bytes,sizeof bytes);if(n<0&&errno==EINTR)continue;if(n<0){rc=-1;break;}if(!n)break;
        copied+=(size_t)n;if(copied>(size_t)st.st_size||write_all_fd(out,bytes,(size_t)n))rc=-1;}
    if(!rc&&(copied!=(size_t)st.st_size||fsync(out)))rc=-1;close(in);if(close(out))rc=-1;
    if(!rc&&link(temp,destination)){if(errno!=EEXIST)rc=-1;else if(lstat(destination,&present)||!S_ISREG(present.st_mode))rc=-1;}
    unlink(temp);
    if(!rc){char parent[REBOX_PATH_MAX];snprintf(parent,sizeof parent,"%s",destination);char *slash=strrchr(parent,'/');if(!slash)return -1;*slash=0;int fd=open(parent,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return -1;rc=fsync(fd);close(fd);}
    return rc;
}
int rb_module_copy_defaults(const char *source,const char *destination){unsigned entries=0;size_t total=0;return seed_tree(source,destination,&entries,&total,0);}
int rb_module_seed(const char *package_subdir,const char *data_subdir){
    const char *package=getenv("REBOX_MODULE_PACKAGE"),*data=getenv("REBOX_MODULE_DATA");if(!package||!data)return 0;
    if(!rb_relative(package_subdir)||(*data_subdir&&!rb_relative(data_subdir)))return fail("invalid module defaults path");
    char from[REBOX_PATH_MAX],to[REBOX_PATH_MAX];if(rb_path(from,sizeof from,package,package_subdir,NULL))return -1;
    if(*data_subdir){if(rb_path(to,sizeof to,data,data_subdir,NULL))return -1;}else if(snprintf(to,sizeof to,"%s",data)>=(int)sizeof to)return -1;
    return rb_module_copy_defaults(from,to);
}
