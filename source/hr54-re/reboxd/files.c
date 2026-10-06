#include "core.h"
int rb_id(const char *s) {
    size_t n=s?strlen(s):0;
    if (!n || n>=REBOX_MODULE_ID_MAX || !strchr("abcdefghijklmnopqrstuvwxyz0123456789", *s)) return 0;
    return strspn(s,"abcdefghijklmnopqrstuvwxyz0123456789._-")==n;
}
int rb_relative(const char *s) {
    if (!s || !*s || *s=='/' || strlen(s)>255) return 0;
    const char *p=s;
    do {
        size_t n=strcspn(p,"/");
        if (!n || (n==1&&*p=='.') || (n==2&&!memcmp(p,"..",2))) return 0;
        for(size_t i=0;i<n;i++) if ((unsigned char)p[i]<32 || p[i]=='\\') return 0;
        p+=n; if(!*p) return 1; p++;
    } while(*p);
    return 0;
}
int rb_path(char *out,size_t cap,const char *root,const char *area,const char *id) {
    int n=snprintf(out,cap,"%s/%s%s%s",root,area,id&&*id?"/":"",id?id:"");
    return n<0||(size_t)n>=cap?-1:0;
}
int rb_mkdir(const char *path) {
    struct stat st;
    if(mkdir(path,0700)&&errno!=EEXIST) return -1;
    if(lstat(path,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()) return -1;
    return chmod(path,0700);
}
char *rb_read(const char *path,size_t cap,size_t *len) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC); struct stat st;
    if(fd<0)return NULL;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||(uint64_t)st.st_size>cap){close(fd);return NULL;}
    size_t size=(size_t)st.st_size,got=0; char *p=malloc(size+1);
    if(!p){close(fd);return NULL;}
    while(got<size){ssize_t n=read(fd,p+got,size-got);if(n<0&&errno==EINTR)continue;if(n<=0){free(p);close(fd);return NULL;}got+=(size_t)n;}
    close(fd);p[size]=0;if(len)*len=size;return p;
}
int rb_atomic(const char *path,const void *p,size_t len) {
    char next[REBOX_PATH_MAX];
    if(snprintf(next,sizeof next,"%s.next",path)>=(int)sizeof next)return -1;
    int fd=open(next,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return -1;
    int rc=fchmod(fd,0600)||write_all_fd(fd,p,len)||fsync(fd);if(close(fd))rc=-1;
    if(!rc)rc=rename(next,path);
    if(rc)unlink(next);
    else {char dir[REBOX_PATH_MAX];snprintf(dir,sizeof dir,"%s",path);char *end=strrchr(dir,'/');if(end){*end=0;fd=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd>=0){fsync(fd);close(fd);}}}
    return rc;
}
void rb_log(const char *id,const char *event) {
    /* Callers pass fixed events and validated IDs; never request bodies or URLs. */
    fprintf(stderr,"reboxd: %s %s\n",id?id:"core",event);
}
