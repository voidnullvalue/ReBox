#include "http.h"
#include <arpa/inet.h>
#include <sys/un.h>
#include <sys/time.h>
int rb_http_read(int fd,RbRequest *r) {
    char header[8193];size_t used=0;memset(r,0,sizeof *r);
    struct timeval tv={5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);
    /* Read through the header boundary without consuming a stream body. */
    while(used<sizeof header-1){ssize_t n=read(fd,header+used,1);if(n<0&&errno==EINTR)continue;if(n!=1)return 400;used++;if(used>=4&&!memcmp(header+used-4,"\r\n\r\n",4))break;}
    if(used==sizeof header-1)return 431;header[used]=0;
    char version[16],extra;char *line=strstr(header,"\r\n");if(!line)return 400;*line=0;
    if(sscanf(header,"%7s %2047s %15s %c",r->method,r->path,version,&extra)!=3||
       (strcmp(version,"HTTP/1.0")&&strcmp(version,"HTTP/1.1"))||*r->path!='/')return 400;
    if(strcmp(r->method,"GET")&&strcmp(r->method,"POST")&&strcmp(r->method,"DELETE"))return 405;
    int seen_length=0,seen_auth=0;
    for(line+=2;*line;) {
        char *end=strstr(line,"\r\n");if(!end)return 400;*end=0;
        if(!strncasecmp(line,"Content-Length:",15)) {
            if(seen_length++)return 400;char *v=line+15;while(*v==' ')v++;
            if(!*v||strspn(v,"0123456789")!=strlen(v)||strlen(v)>8)return 400;
            unsigned long n=strtoul(v,NULL,10);if(n>RB_BODY_MAX)return 413;r->length=n;
        } else if(!strncasecmp(line,"Authorization:",14)) {
            if(seen_auth++)return 400;char *v=line+14;while(*v==' ')v++;
            if(strlen(v)>=sizeof r->authorization)return 400;strcpy(r->authorization,v);
        } else if(!strncasecmp(line,"Transfer-Encoding:",18))return 400;
        line=end+2;
    }
    size_t got=0;while(got<r->length){ssize_t n=read(fd,r->body+got,r->length-got);if(n<0&&errno==EINTR)continue;if(n<=0)return 400;got+=(size_t)n;}
    if(memchr(r->body,0,r->length))return 400;r->body[r->length]=0;return 0;
}
void rb_http_reply(int fd,int code,const char *type,const void *body,size_t n) {
    char h[512];int k=snprintf(h,sizeof h,"HTTP/1.0 %d Response\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",code,type,n);
    if(k>0&&(size_t)k<sizeof h&&!write_all_fd(fd,h,(size_t)k))write_all_fd(fd,body,n);
}
void rb_http_json(int fd,int code,const char *body){rb_http_reply(fd,code,"application/json",body,strlen(body));}
void rb_http_error(int fd,int code,const char *error){struct sb b={0};sb_puts(&b,"{\"ok\":false,\"error\":");sb_json_str(&b,error);sb_puts(&b,"}");rb_http_json(fd,code,b.p);free(b.p);}
int rb_listen_tcp(int port){int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -1;int one=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=htonl(INADDR_ANY)};if(bind(fd,(void *)&a,sizeof a)||listen(fd,16)){close(fd);return -1;}return fd;}
int rb_listen_unix(const char *path){struct sockaddr_un a={.sun_family=AF_UNIX};if(strlen(path)>=sizeof a.sun_path)return -1;strcpy(a.sun_path,path);int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);if(fd<0)return -1;if(bind(fd,(void *)&a,sizeof a)||chmod(path,0600)||listen(fd,8)){close(fd);return -1;}return fd;}
