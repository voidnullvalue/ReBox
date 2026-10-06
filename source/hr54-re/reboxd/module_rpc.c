#include "module_rpc.h"
#include <sys/un.h>
#include <sys/time.h>
int rb_rpc_open(const ReboxModule *m,const char *method,const char *path,const char *body,int timeout) {
    if(!m->pid||!*m->socket||*path!='/'||strlen(path)>2047||strpbrk(path,"\r\n "))return -1;
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -1;
    struct timeval tv={timeout,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);
    struct sockaddr_un a={.sun_family=AF_UNIX};snprintf(a.sun_path,sizeof a.sun_path,"%s",m->socket);
    if(connect(fd,(void *)&a,sizeof a)){close(fd);return -1;}
    struct sb req={0};sb_fmt(&req,"%s %s HTTP/1.0\r\nHost: module\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",method,path,body?strlen(body):0);if(body)sb_puts(&req,body);
    int rc=write_all_fd(fd,req.p,req.len);free(req.p);if(rc){close(fd);return -1;}return fd;
}
void rb_reply_free(RbReply *r){free(r->body.p);memset(r,0,sizeof *r);}
int rb_rpc(const ReboxModule *m,const char *method,const char *path,const char *body,RbReply *r,int timeout) {
    memset(r,0,sizeof *r);int fd=rb_rpc_open(m,method,path,body,timeout);if(fd<0)return fail("module unavailable");
    char h[8193];size_t n=0;int rc=-1;
    while(n<sizeof h-1){ssize_t got=read(fd,h+n,1);if(got<0&&errno==EINTR)continue;if(got!=1)goto done;n++;if(n>=4&&!memcmp(h+n-4,"\r\n\r\n",4))break;}
    if(n==sizeof h-1)goto done;h[n]=0;char version[16];if(sscanf(h,"%15s %d",version,&r->status)!=2||r->status<200||r->status>599)goto done;
    if(strcmp(version,"HTTP/1.0")&&strcmp(version,"HTTP/1.1"))goto done;
    long length=-1;char *p=strstr(h,"\r\n");
    while(p&&p[2]){p+=2;char *end=strstr(p,"\r\n");if(!end)break;
        if(!strncasecmp(p,"Content-Length:",15)){if(length!=-1)goto done;char *v=p+15;while(*v==' ')v++;char *stop;errno=0;length=strtol(v,&stop,10);if(errno||stop!=end||length<0||length>RB_RESPONSE_MAX)goto done;}
        if(!strncasecmp(p,"Transfer-Encoding:",18))goto done;
        if(!strncasecmp(p,"Content-Type:",13)){char *v=p+13;while(*v==' ')v++;size_t k=end-v;if(k>=sizeof r->type)goto done;memcpy(r->type,v,k);r->type[k]=0;}
        p=end;
    }
    if(length<0)goto done;
    while(r->body.len<(size_t)length){char b[16384];size_t want=(size_t)length-r->body.len;if(want>sizeof b)want=sizeof b;ssize_t k=read(fd,b,want);if(k<0&&errno==EINTR)continue;if(k<=0||sb_putn(&r->body,b,k))goto done;}
    if(!r->body.p)sb_puts(&r->body,"");rc=0;
done:close(fd);if(rc){rb_reply_free(r);return fail("invalid or timed out module response");}return 0;
}
int rb_rpc_health(const ReboxModule *m) {
    RbReply r;if(rb_rpc(m,"GET","/status",NULL,&r,1))return -1;
    struct jval *v=json_parse(r.body.p,r.body.len);int ok=r.status==200&&jbool(jget(v,"ok"),0)&&jnum(jget(v,"moduleApi"),0)==1;
    jfree(v);rb_reply_free(&r);return ok?0:-1;
}
