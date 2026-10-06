#include "../shared/sdk.h"
#include "../../reboxd/module_auth.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/time.h>
#define REBOX_FRIGATE_MODULE 1
static const char *data_root;
static char host[64]="192.168.88.39",session[33];static int port=5000,playing;
static pthread_mutex_t operation_mutex=PTHREAD_MUTEX_INITIALIZER,state_mutex=PTHREAD_MUTEX_INITIALIZER;
/* JSON is bounded and contains private camera URLs. Only normalized names and
 * codec decisions may leave this process. */
static struct jval *frigate_get(const char *path){
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return NULL;struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(port)};
    struct timeval timeout={5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
    if(inet_pton(AF_INET,host,&address.sin_addr)!=1||connect(fd,(void *)&address,sizeof address)){close(fd);fail("Frigate connection failed");return NULL;}
    struct sb request={0},response={0};sb_fmt(&request,"GET %s HTTP/1.0\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",path,host,port);
    int rc=write_all_fd(fd,request.p,request.len);free(request.p);while(!rc){char b[16384];ssize_t n=read(fd,b,sizeof b);if(n<0&&errno==EINTR)continue;if(n<0){rc=-1;break;}if(!n)break;if(response.len+(size_t)n>2*1024*1024||sb_putn(&response,b,n)){rc=-1;break;}}close(fd);
    struct jval *v=NULL;int code=0;char *body=response.p?strstr(response.p,"\r\n\r\n"):NULL;if(!rc&&body&&sscanf(response.p,"HTTP/%*s %d",&code)==1&&code==200)v=json_parse(body+4,response.len-(size_t)(body+4-response.p));free(response.p);if(!v)fail("Frigate API unavailable");return v;
}
#include "library.inc"
static int configuration(struct jval *v){
    const char *value=jstr(jget(v,"host"));double number=jnum(jget(v,"port"),0);struct in_addr ipv4;
    if(!value||strlen(value)>=sizeof host||inet_pton(AF_INET,value,&ipv4)!=1||number<1||number>65535||number!=(int)number)return fail("An IPv4 host and valid port are required");
    strcpy(host,value);port=(int)number;return 0;
}
static int browse(const char *query,int search,struct sb *out){
    struct sb raw={0};if(frigate_cameras(&raw))return -1;struct jval *v=json_parse(raw.p,raw.len),*rows=jget(v,"cameras");free(raw.p);if(!rows||rows->t!=J_ARR){jfree(v);return fail("invalid camera list");}
    char q[256]="",num[32]="";query_param(query,"q",q,sizeof q);query_param(query,"offset",num,sizeof num);int offset=atoi(num);num[0]=0;query_param(query,"limit",num,sizeof num);int limit=*num?atoi(num):60;
    if(offset<0||offset>10000||limit<1||limit>60){jfree(v);return fail("invalid page");}
    int total=0,count=0;sb_puts(out,"{\"ok\":true,\"items\":[");
    for(size_t i=0;i<rows->n;i++){struct jval *camera=rows->items[i];const char *id=jstr(jget(camera,"id"));if(!id||strlen(id)>255)continue;if(search&&*q&&!strcasestr(id,q))continue;int at=total++;if(at<offset||count>=limit)continue;if(count++)sb_puts(out,",");sb_puts(out,"{\"id\":");sb_json_str(out,id);sb_puts(out,",\"title\":");sb_json_str(out,id);sb_fmt(out,",\"kind\":\"item\",\"playable\":%s,\"description\":",jbool(jget(camera,"playable"),0)?"true":"false");sb_json_str(out,jstr(jget(camera,"reason")));sb_puts(out,"}");}
    sb_fmt(out,"],\"total\":%d,\"offset\":%d,\"hasMore\":%s}",total,offset,offset+count<total?"true":"false");jfree(v);return 0;
}
static int prepare(struct jval *body,struct sb *out){
    const char *id=jstr(jget(body,"itemId"));if(!id||!*id||strlen(id)>255)return fail("camera ID required");struct jval *v=frigate_get("/api/config");if(!v)return -1;char stream[256];int rc=frigate_camera(v,id,stream,sizeof stream);jfree(v);if(rc)return -1;
    struct sb url={0};sb_fmt(&url,"http://%s:%d/stream.ts?src=",host,port);url_encode(&url,stream);char token[33];if(rb_random(token,32)){free(url.p);return -1;}
    pthread_mutex_lock(&state_mutex);strcpy(session,token);playing=1;pthread_mutex_unlock(&state_mutex);
    sb_puts(out,"{\"ok\":true,\"type\":\"stream\",\"title\":");sb_json_str(out,id);sb_puts(out,",\"live\":true,\"duration\":0,\"stream\":{\"kind\":\"http\",\"url\":");sb_json_str(out,url.p);sb_fmt(out,"},\"session\":\"%s\",\"transport\":{\"stop\":true,\"pause\":false,\"resume\":false,\"seek\":false}}",token);free(url.p);return 0;
}
static void settings(struct sb *out){sb_puts(out,"{\"ok\":true,\"fields\":[{\"key\":\"host\",\"label\":\"Frigate IPv4 host\",\"type\":\"string\",\"value\":");sb_json_str(out,host);sb_fmt(out,"},{\"key\":\"port\",\"label\":\"HTTP port\",\"type\":\"integer\",\"value\":%d}],\"actions\":[]}",port);}
static void handle(int fd,const RbRequest *request){
    char path[2048];strcpy(path,request->path);char *query=strchr(path,'?');if(query)*query++=0;
    if(!strcmp(path,"/status")){pthread_mutex_lock(&state_mutex);struct sb out={0};sb_fmt(&out,"{\"ok\":true,\"moduleApi\":1,\"playback\":{\"session\":\"%s\",\"playing\":%s}}",session,playing?"true":"false");pthread_mutex_unlock(&state_mutex);rb_http_json(fd,200,out.p);free(out.p);return;}
    if(!strcmp(path,"/playback/stop")){struct jval *v=json_parse(request->body,request->length);const char *token=jstr(jget(v,"session"));pthread_mutex_lock(&state_mutex);if(!token||!strcmp(token,session))playing=0;pthread_mutex_unlock(&state_mutex);jfree(v);rb_http_json(fd,200,"{\"ok\":true}");return;}
    pthread_mutex_lock(&operation_mutex);struct sb out={0};struct jval *body=json_parse(*request->body?request->body:"{}",*request->body?request->length:2);int rc=0,code=200;
    if(!body||body->t!=J_OBJ)code=400;
    else if(!strcmp(path,"/browse")||!strcmp(path,"/search"))rc=browse(query,!strcmp(path,"/search"),&out);
    else if(!strcmp(path,"/legacy/cameras"))rc=frigate_cameras(&out);
    else if(!strcmp(path,"/play"))rc=prepare(body,&out);
    else if(!strcmp(path,"/settings")){
        if(!strcmp(request->method,"POST")){
            const char *next=jstr(jget(body,"host"));struct jval *number=jget(body,"port");if(body->n!=1||(!next&&!number)||(number&&(number->t!=J_NUM||number->num<1||number->num>65535||number->num!=(int)number->num)))code=400;
            else{struct sb config={0};sb_puts(&config,"{\"host\":");sb_json_str(&config,next?next:host);sb_fmt(&config,",\"port\":%.0f}",number?jnum(number,0):port);struct jval *v=json_parse(config.p,config.len);char old[64];strcpy(old,host);int old_port=port;
                if(configuration(v))code=400;else{char file[1024];snprintf(file,sizeof file,"%s/config.json",data_root);rc=rb_atomic(file,config.p,config.len);if(rc){strcpy(host,old);port=old_port;}}jfree(v);free(config.p);}}
        if(!rc&&code==200)settings(&out);
    }else code=404;
    if(rc)rb_http_error(fd,502,g_err);else if(code!=200)rb_http_error(fd,code,"invalid module operation");else rb_http_json(fd,200,out.p?out.p:"{}");jfree(body);free(out.p);pthread_mutex_unlock(&operation_mutex);
}
int main(void){data_root=getenv("REBOX_MODULE_DATA");if(!data_root)return 2;char file[1024];size_t n;snprintf(file,sizeof file,"%s/config.json",data_root);char *raw=rb_read(file,16384,&n);struct jval *v=raw?json_parse(raw,n):NULL;if(v)configuration(v);jfree(v);free(raw);return rb_module_serve(handle);}
