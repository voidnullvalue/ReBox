#include "package.h"
#include <dirent.h>
/* Paths are core-created staging files. No shell interprets archive data. */
static pid_t command_pipe(int *fd,const char *exe,char *const argv[]) {
    int p[2];if(pipe2(p,O_CLOEXEC))return -1;pid_t pid=fork();
    if(!pid){dup2(p[1],1);for(int i=3;i<65536;i++)close(i);char *env[]={"PATH=/bin:/usr/bin",NULL};execve(exe,argv,env);_exit(127);}
    close(p[1]);if(pid<0){close(p[0]);return -1;}*fd=p[0];return pid;
}
static int finish(pid_t pid,int fd){close(fd);int status;pid_t rc;do{rc=waitpid(pid,&status,0);}while(rc<0&&errno==EINTR);return rc==pid&&WIFEXITED(status)&&!WEXITSTATUS(status)?0:-1;}
int rb_package_sha(const char *path,char sha[65]) {
#ifdef REBOX_RECEIVER
    char *argv[]={"/bin/busybox","sha256sum",(char *)path,NULL};
#else
    char *argv[]={"/usr/bin/sha256sum",(char *)path,NULL};
#endif
    int fd;pid_t pid=command_pipe(&fd,argv[0],argv);if(pid<0)return -1;char data[2048];ssize_t n=read(fd,data,sizeof data);int rc=finish(pid,fd);if(rc||n<65||data[64]!=' '||strspn(data,"0123456789abcdef")!=64)return -1;memcpy(sha,data,64);sha[64]=0;return 0;
}
static int read_exact(int fd,void *bytes,size_t n){char *p=bytes;while(n){struct pollfd pollfd={fd,POLLIN,0};if(poll(&pollfd,1,5000)<=0)return -1;ssize_t k=read(fd,p,n);if(k<0&&errno==EINTR)continue;if(k<=0)return -1;p+=k;n-=k;}return 0;}
static int octal(const char *s,size_t n,uint64_t *out){size_t i=0;uint64_t v=0;while(i<n&&s[i]==' ')i++;int digits=0;while(i<n&&s[i]>='0'&&s[i]<='7'){if(v>UINT64_MAX/8)return -1;v=v*8+s[i++]-'0';digits++;}while(i<n)if(s[i]&&s[i]!=' ')return -1;else i++;*out=v;return digits?0:-1;}
static int zero(const unsigned char *p,size_t n){for(size_t i=0;i<n;i++)if(p[i])return 0;return 1;}
static int parents(const char *root,const char *rel){char path[REBOX_PATH_MAX];if(rb_path(path,sizeof path,root,rel,NULL))return -1;for(char *p=path+strlen(root)+1;*p;p++)if(*p=='/'){*p=0;int rc=rb_mkdir(path);*p='/';if(rc)return -1;}return 0;}
int rb_package_extract(const char *archive,const char *dir) {
    struct stat st;if(lstat(archive,&st)||!S_ISREG(st.st_mode)||st.st_size<0||st.st_size>RB_PACKAGE_MAX)return fail("package exceeds limit");
#ifdef REBOX_RECEIVER
    char *argv[]={"/bin/busybox","gzip","-dc",(char *)archive,NULL};
#else
    char *argv[]={"/bin/gzip","-dc",(char *)archive,NULL};
#endif
    int fd;pid_t pid=command_pipe(&fd,argv[0],argv);if(pid<0)return -1;
    char (*seen)[256]=calloc(4096,256);size_t count=0;uint64_t total=0;int rc=-1;
    if(!seen)goto done;
    for(;;){unsigned char h[512];if(read_exact(fd,h,sizeof h))goto done;
        if(zero(h,sizeof h)){if(read_exact(fd,h,sizeof h)||!zero(h,sizeof h))goto done;/* Reject concatenated archives and excessive padding. */
            unsigned char tail[512];size_t padding=0;ssize_t n;while((n=read(fd,tail,sizeof tail))>0){padding+=n;if(padding>10240||!zero(tail,n))goto done;}if(n<0)goto done;rc=0;break;}
        if(count==4096||memcmp(h+257,"ustar",5)||h[262]!=0)goto done;
        uint64_t expected,size,mode;unsigned sum=0;for(size_t i=0;i<512;i++)sum+=(i>=148&&i<156)?32:h[i];
        if(octal((char *)h+148,8,&expected)||expected!=sum||octal((char *)h+124,12,&size)||octal((char *)h+100,8,&mode)||size>RB_EXTRACT_MAX-total)goto done;
        if(h[156]!=0&&h[156]!='0'&&h[156]!='5')goto done;
        if(h[157]||!memchr(h,0,100)||!memchr(h+345,0,155))goto done;
        char rel[256],path[REBOX_PATH_MAX];int k=snprintf(rel,sizeof rel,"%s%s%s",h+345,h[345]?"/":"",h);
        if(k<0||k>=256)goto done;size_t len=strlen(rel);if(h[156]=='5'&&len&&rel[len-1]=='/')rel[--len]=0;
        if(!rb_relative(rel))goto done;for(size_t i=0;i<count;i++)if(!strcmp(seen[i],rel))goto done;strcpy(seen[count++],rel);
        if(parents(dir,rel)||rb_path(path,sizeof path,dir,rel,NULL))goto done;
        if(h[156]=='5'){if(size||rb_mkdir(path))goto done;continue;}
        int out=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,mode&0111?0700:0600);if(out<0)goto done;
        uint64_t remaining=size;total+=size;int bad=0;
        while(remaining){char block[32768];size_t n=remaining>sizeof block?sizeof block:(size_t)remaining;if(read_exact(fd,block,n)||write_all_fd(out,block,n)){bad=1;break;}remaining-=n;}
        if(!bad&&fsync(out))bad=1;if(close(out))bad=1;if(bad)goto done;
        size_t pad=(512-size%512)%512;if(pad){char block[512];if(read_exact(fd,block,pad))goto done;}
    }
done:free(seen);if(rc)kill(pid,SIGKILL);if(finish(pid,fd))rc=-1;return rc?fail("invalid or unsafe module archive"):0;
}
static int url_valid(const char *s) {
    if(!s||strlen(s)>2048)return 0;const char *host=!strncmp(s,"https://",8)?s+8:!strncmp(s,"http://",7)?s+7:NULL;if(!host||!*host)return 0;
    const char *end=host+strcspn(host,"/?#");if(end==host||memchr(host,'@',end-host))return 0;
    for(const char *p=s;*p;p++)if((unsigned char)*p<=32||(unsigned char)*p==127||*p=='\\')return 0;return 1;
}
int rb_download(const char *url,const char *path) {
    if(!url_valid(url))return fail("invalid module URL");
#ifdef REBOX_HOST_TEST
    const char *helper="./build/module-fetch";
#else
    const char *helper="/var/hr54-persist/rebox/bin/module-fetch";
#endif
    char *argv[]={(char *)helper,(char *)url,NULL};int in;pid_t pid=command_pipe(&in,helper,argv);if(pid<0)return -1;
    int out=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);size_t total=0;int rc=out<0?-1:0;double deadline=mono_now()+180;
    while(!rc){struct pollfd p={in,POLLIN,0};if(mono_now()>deadline||poll(&p,1,5000)<=0){rc=-1;break;}char data[32768];ssize_t n=read(in,data,sizeof data);if(n<0&&errno==EINTR)continue;if(n<0){rc=-1;break;}if(!n)break;
        if((size_t)n>RB_PACKAGE_MAX-total||write_all_fd(out,data,n)){rc=-1;break;}total+=n;}
    if(out>=0){if(fsync(out))rc=-1;close(out);}if(rc)kill(pid,SIGKILL);if(finish(pid,in))rc=-1;if(!total)rc=-1;if(rc)unlink(path);return rc?fail("module download failed"):0;
}
int rb_remove_tree(const char *path){struct stat st;if(lstat(path,&st))return errno==ENOENT?0:-1;if(!S_ISDIR(st.st_mode))return unlink(path);DIR *d=opendir(path);if(!d)return -1;int rc=0;struct dirent *e;while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;char sub[REBOX_PATH_MAX];if(rb_path(sub,sizeof sub,path,e->d_name,NULL)||rb_remove_tree(sub)){rc=-1;break;}}closedir(d);return rc?rc:rmdir(path);}
