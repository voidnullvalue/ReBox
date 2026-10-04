#include "client.h"
void api_init(ApiClient *a,int port){memset(a,0,sizeof(*a));a->port=port;for(int i=0;i<API_SLOTS;i++)a->r[i].fd=-1;}
void api_cancel(ApiClient *a,int i){ApiRequest *r=&a->r[i];if(r->fd>=0)close(r->fd);free(r->data);memset(r,0,sizeof(*r));r->fd=-1;}
void api_close(ApiClient *a){for(int i=0;i<API_SLOTS;i++)api_cancel(a,i);}
int api_busy(const ApiClient *a,int i){return a->r[i].kind!=API_NONE;}
static int connect_request(ApiClient *a,ApiRequest *r,uint64_t now){r->fd=socket(AF_INET,SOCK_STREAM,0);if(r->fd<0)return -1;int f=fcntl(r->fd,F_GETFL,0);if(f<0||fcntl(r->fd,F_SETFL,f|O_NONBLOCK)<0)return -1;fcntl(r->fd,F_SETFD,FD_CLOEXEC);struct sockaddr_in addr={0};addr.sin_family=AF_INET;unsigned char *p=(unsigned char *)&addr.sin_port;p[0]=a->port>>8;p[1]=a->port;unsigned char *ip=(unsigned char *)&addr.sin_addr;ip[0]=127;ip[3]=1;int rc=connect(r->fd,(const struct sockaddr *)&addr,sizeof(addr));if(rc<0&&errno!=EINPROGRESS)return -1;r->phase=0;r->sent=r->used=r->header=r->body_length=0;r->has_length=0;r->status=0;r->deadline=now+r->timeout;return 0;}
int api_send(ApiClient *a,int slot,ApiKind kind,const char *method,const char *path,const char *body,unsigned timeout){
    if(slot<0||slot>=API_SLOTS||!path||path[0]!='/'||strchr(path,'\r')||strchr(path,'\n')||strlen(path)>1700)return -1;
    if(api_busy(a,slot)){if(slot==API_BROWSE||slot==API_ARTWORK)api_cancel(a,slot);else return -1;}
    ApiRequest *r=&a->r[slot];r->kind=kind;r->slot=slot;r->generation=a->generation;r->timeout=timeout;r->is_get=!strcmp(method,"GET");r->cap=kind==API_IMAGE?2*1024*1024:512*1024;r->data=malloc(r->cap+1);if(!r->data){api_cancel(a,slot);return -1;}
    int n=snprintf(r->request,sizeof(r->request),"%s %s HTTP/1.0\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: %u\r\n\r\n%s",method,path,a->port,(unsigned)(body?strlen(body):0),body?body:"");
    if(n<0||(size_t)n>=sizeof(r->request)){api_cancel(a,slot);return -1;}r->size=n;uint64_t now=ui_now();if(connect_request(a,r,now)){if(r->fd>=0)close(r->fd);r->fd=-1;r->retry_at=now+200;r->phase=3;}return 0;
}
static void complete(ApiClient *a,ApiRequest *r,ApiError e,ApiSink sink,void *ctx){if(e==API_TIMEOUT&&getenv("HR54_INPUT_TRACE"))fprintf(stderr,"http-trace: timeout kind=%d used=%u header=%u body=%u expected=%u framed=%d\n",r->kind,(unsigned)r->used,(unsigned)r->header,(unsigned)(r->used-r->header),(unsigned)r->body_length,r->has_length);ApiResponse response={r->kind,e,r->generation,r->status,(unsigned char *)r->data+r->header,r->used-r->header};int slot=r->slot;sink(ctx,&response);/* callbacks may enqueue other slots, but this slot stays occupied until return */api_cancel(a,slot);}
/* Content-Length is the response boundary, not socket EOF. A shell child may
 * retain the server descriptor; never hold a complete state reply hostage. */
static int parse_length(ApiRequest *r){
    const char *line=strstr(r->data,"\r\n");if(!line)return -1;
    line+=2;while(line<r->data+r->header-2){const char *end=strstr(line,"\r\n");if(!end)return -1;
        if((size_t)(end-line)>=15&&!strncasecmp(line,"Content-Length:",15)){
            const char *p=line+15;while(p<end&&(*p==' '||*p=='\t'))p++;size_t value=0;int digits=0;
            while(p<end&&*p>='0'&&*p<='9'){unsigned digit=(unsigned)(*p++-'0');if(value>r->cap/10||(value==r->cap/10&&digit>r->cap%10))return -1;value=value*10+digit;digits++;}
            while(p<end&&(*p==' '||*p=='\t'))p++;if(!digits||p!=end||value>r->cap-r->header||(r->has_length&&value!=r->body_length))return -1;
            r->has_length=1;r->body_length=value;
        }line=end+2;
    }return 0;
}
static ApiError status_result(int status){return status>=200&&status<300?API_OK:status==401||status==403?API_UNAUTHORIZED:API_FAILED;}
void api_pump(ApiClient *a,uint64_t now,ApiSink sink,void *ctx){
    for(int i=0;i<API_SLOTS;i++){ApiRequest *r=&a->r[i];if(!r->kind)continue;
        if(r->phase==3){if(now<r->retry_at)continue;if(r->attempt++>=2||connect_request(a,r,now)){complete(a,r,API_UNAVAILABLE,sink,ctx);continue;}}
        if(now>=r->deadline){complete(a,r,API_TIMEOUT,sink,ctx);continue;}
        struct pollfd p={r->fd,r->phase<2?POLLOUT:POLLIN,0};int ready=poll(&p,1,0);if(ready<=0)continue;
        if(r->phase==0){int error=0;unsigned n=sizeof(error);if(getsockopt(r->fd,SOL_SOCKET,SO_ERROR,&error,&n)||error)goto disconnected;r->phase=1;}
        if(r->phase==1){ssize_t n=send(r->fd,r->request+r->sent,r->size-r->sent,MSG_NOSIGNAL);if(n<0&&(errno==EAGAIN||errno==EINTR))continue;if(n<=0)goto disconnected;r->sent+=n;if(r->sent==r->size)r->phase=2;continue;}
        /* Bound each pump to 64 KiB so artwork cannot starve key handling. */
        size_t room=r->cap-r->used;if(!room){complete(a,r,API_MALFORMED,sink,ctx);continue;}if(room>65536)room=65536;
        ssize_t n=recv(r->fd,r->data+r->used,room,0);if(n<0&&(errno==EAGAIN||errno==EINTR))continue;if(n<0)goto disconnected;
        if(n>0){r->used+=n;r->data[r->used]=0;if(!r->header){char *end=strstr(r->data,"\r\n\r\n");if(end){r->header=(size_t)(end-r->data)+4;if(r->header>8192||sscanf(r->data,"HTTP/%*s %d",&r->status)!=1||parse_length(r)){complete(a,r,API_MALFORMED,sink,ctx);continue;}}else if(r->used>8192){complete(a,r,API_MALFORMED,sink,ctx);continue;}}if(r->header&&r->has_length){size_t body=r->used-r->header;if(body>r->body_length){complete(a,r,API_MALFORMED,sink,ctx);continue;}if(body==r->body_length)complete(a,r,status_result(r->status),sink,ctx);}continue;}
        if(!r->header){complete(a,r,API_MALFORMED,sink,ctx);continue;}
        if(r->has_length&&r->body_length!=r->used-r->header){complete(a,r,API_MALFORMED,sink,ctx);continue;}
        complete(a,r,status_result(r->status),sink,ctx);continue;
        disconnected:
        if(r->is_get&&r->attempt<2){close(r->fd);r->fd=-1;r->phase=3;r->retry_at=now+200*(r->attempt+1);}else complete(a,r,API_UNAVAILABLE,sink,ctx);
    }
}
int api_encode(char *d,size_t cap,const char *s){size_t n=0;const char *hex="0123456789ABCDEF";for(;*s;s++){unsigned char c=*s;int safe=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||strchr("-_.~",c);if(n+(safe?1:3)>=cap)return -1;if(safe)d[n++]=c;else{d[n++]='%';d[n++]=hex[c>>4];d[n++]=hex[c&15];}}d[n]=0;return 0;}
const char *api_error_message(ApiError e){switch(e){case API_UNAVAILABLE:return "The receiver service is unavailable.";case API_TIMEOUT:return "This is taking longer than expected.";case API_UNAUTHORIZED:return "Connect your account to continue.";case API_MALFORMED:return "The service returned an unreadable response.";default:return "We could not complete that request.";}}
