#include "dispatcher.h"
static uint32_t word(const unsigned char *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void put(unsigned char *p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static const uint32_t keys[]={0xe000,0xe001,0xe002,0xe006,0xe007,0xe00e,0xe100,0xe101,0xe102,0xe103,0xe200,0xe201,0xe202,0xe203,0xe300,0xe301,0xe302,0xe303,0xe304,0xe305,0xe306,0xe307,0xe308,0xe309,0xe400,0xe401,0xe402,0xe403,0xe405,0xe406,0xe408,0xe409,0xe500,0xe504,0xe505,0xe506,0xe507,0xe508,0xe60a};
static const uint32_t transport[]={0xe006,0xe007,0xe00e,0xe400,0xe401,0xe402,0xe405,0xe406,0xe408,0xe409};
static const uint32_t special[]={0xe00b,0xe501,0xe502,0xe503};
static int requested(const Input *in,uint32_t raw){
    if(in->manual!=1)for(size_t i=in->manual==2?0:3;i<sizeof(special)/sizeof(*special);i++)if(raw==special[i])return 1;
    if(in->registered_mode==2){for(size_t i=0;i<sizeof(transport)/sizeof(*transport);i++)if(raw==transport[i])return 1;}
    else if(in->registered_mode)for(size_t i=0;i<sizeof(keys)/sizeof(*keys);i++)if(raw==keys[i])return 1;
    return 0;
}
static void map(Input *in,int session){
    size_t n=0;if(session){put(in->tx,12);put(in->tx+4,16);put(in->tx+8,0);n=12;}size_t start=n;n+=20;
    /* Navigation needs per-key exclusivity. The backend prepares the shell
     * before acquisition because a stock modal may already own these keys. */
    size_t shared=0;
    size_t exclusive_at=n;n+=4;size_t exclusive=0;
    if(in->manual!=1)for(size_t i=in->manual==2?0:3;i<sizeof(special)/sizeof(*special);i++){put(in->tx+n,special[i]);n+=4;exclusive++;}
    if(in->wanted_active==2)for(size_t i=0;i<sizeof(transport)/sizeof(*transport);i++){put(in->tx+n,transport[i]);n+=4;exclusive++;}
    else if(in->wanted_active)for(size_t i=0;i<sizeof(keys)/sizeof(*keys);i++){put(in->tx+n,keys[i]);n+=4;exclusive++;}
    put(in->tx+start,n-start);put(in->tx+start+4,0);put(in->tx+start+8,in->owner);put(in->tx+start+12,1);put(in->tx+start+16,shared+1);put(in->tx+exclusive_at,exclusive+1);
    fprintf(stderr,"hr54-ui: registering owner=%u mode=%d shared=%u exclusive=%u\n",in->owner,in->wanted_active,(unsigned)shared,(unsigned)exclusive);
    in->registered_mode=in->wanted_active;in->size=n;in->sent=0;in->phase=2;in->ready=0;in->deadline=ui_now()+3000;memset(in->held,0,sizeof(in->held));
}
static int input_open_port(Input *in,int manual,unsigned port){memset(in,0,sizeof(*in));in->fd=-1;in->manual=manual;in->wanted_active=1;in->fd=socket(AF_INET,SOCK_STREAM,0);if(in->fd<0)return -1;int f=fcntl(in->fd,F_GETFL,0);if(f<0||fcntl(in->fd,F_SETFL,f|O_NONBLOCK)<0){input_close(in);return -1;}fcntl(in->fd,F_SETFD,FD_CLOEXEC);struct sockaddr_in a={0};a.sin_family=AF_INET;unsigned char *p=(unsigned char *)&a.sin_port;p[0]=port>>8;p[1]=port&255;p=(unsigned char *)&a.sin_addr;p[0]=127;p[3]=1;if(connect(in->fd,(const struct sockaddr *)&a,sizeof(a))<0&&errno!=EINPROGRESS){input_close(in);return -1;}in->phase=0;in->deadline=ui_now()+3000;return 0;}
static int traced_open(Input *in,int manual,unsigned port){int rc=input_open_port(in,manual,port);in->port=port;const char *trace=getenv("HR54_INPUT_TRACE");in->trace=trace&&strcmp(trace,"0");return rc;}
int input_open(Input *in,int manual){return traced_open(in,manual,47952);}
int input_open_broker(Input *in,int manual){return traced_open(in,manual==1?1:2,47953);}
int input_mode(Input *in,int active){
    in->wanted_active=active;if(in->fd<0)return -1;
    if(in->ready&&in->active!=active)map(in,0);
    return 0;
}
void input_close(Input *in){if(in->fd>=0){unsigned char bye[12];put(bye,12);put(bye+4,4);put(bye+8,in->owner);send(in->fd,bye,sizeof(bye),MSG_NOSIGNAL);close(in->fd);}in->fd=-1;in->ready=0;memset(in->held,0,sizeof(in->held));}
int input_pump(Input *in,uint64_t now,KeySink sink,void *ctx){
    if(in->fd<0)return -1;if(!in->ready&&now>=in->deadline)goto fail;
    if(in->phase==0){struct pollfd p={in->fd,POLLOUT,0};if(poll(&p,1,0)<=0)return 0;int e=0;unsigned n=sizeof(e);if(getsockopt(in->fd,SOL_SOCKET,SO_ERROR,&e,&n)||e)goto fail;in->phase=1;}
    if(in->phase==2){ssize_t n=send(in->fd,in->tx+in->sent,in->size-in->sent,MSG_NOSIGNAL);if(n<0&&(errno==EAGAIN||errno==EINTR))return 0;if(n<=0)goto fail;in->sent+=n;if(in->sent<in->size)return 0;in->phase=3;/* Empty manual maps have no ACK. */if(in->manual==1&&!in->registered_mode){in->phase=4;in->ready=1;in->active=0;}}
    for(int budget=0;budget<32;budget++){
        if(in->phase==1&&in->used>=4){in->owner=word(in->rx);in->used-=4;memmove(in->rx,in->rx+4,in->used);map(in,1);return 0;}
        if(in->phase>=3&&in->used>=4){uint32_t n=word(in->rx);if(n<8||n>sizeof(in->rx)||n%4)goto fail;if(in->used>=n){unsigned command=word(in->rx+4);
            if(in->trace){fprintf(stderr,"input-trace: port=%u owner=%u frame bytes=%u command=%u words=",in->port,in->owner,n,command);for(unsigned p=0;p<n;p+=4)fprintf(stderr," %08x",word(in->rx+p));fprintf(stderr,"\n");}
            if(command==7){
                if(n<12||word(in->rx+8)!=n/4-2)goto fail;
                /* Selective release can report retired keys. A key still
                 * required by the requested map is a rejection/loss and must
                 * fail closed, whether during acquisition or after readiness. */
                for(unsigned p=12;p<n;p+=4){uint32_t raw=word(in->rx+p)&65535;UiKey k=input_key(raw);if(k)in->held[k]=0;if(requested(in,raw)){fprintf(stderr,"hr54-ui: required key ownership rejected owner=%u mode=%d key=0x%x\n",in->owner,in->registered_mode,raw);goto fail;}}
                if(n>12)fprintf(stderr,"hr54-ui: retired key notice owner=%u mode=%d count=%u\n",in->owner,in->registered_mode,(unsigned)(n/4-3));
                if(in->phase==3){in->ready=1;in->active=in->registered_mode;in->phase=4;fprintf(stderr,"hr54-ui: key map accepted owner=%u active=%d guide=%d\n",in->owner,in->active,in->manual==2);}
            }
            else if(command==8&&(n==12||n==16||n==24||n==28)){uint32_t raw=word(in->rx+8);UiKey k=input_key(raw);if(in->trace)fprintf(stderr,"input-trace: port=%u decoded raw=%08x UiKey=%d pressed=%d repeat=%d\n",in->port,raw,k,!!(raw&0x10000),k&&!!(raw&0x10000)&&in->held[k]);if(k){int pressed=!!(raw&0x10000);KeyEvent ev={k,pressed,pressed&&in->held[k],raw};in->held[k]=pressed;if(sink)sink(ctx,ev);}}
            in->used-=n;memmove(in->rx,in->rx+n,in->used);continue;}}
        if(in->used==sizeof(in->rx))goto fail;ssize_t n=recv(in->fd,in->rx+in->used,sizeof(in->rx)-in->used,0);if(n<0&&(errno==EAGAIN||errno==EINTR))return 0;if(n<=0)goto fail;in->used+=n;
    }return 0;
    fail:fprintf(stderr,"input-trace: failure port=%u owner=%u phase=%d buffered=%u errno=%d\n",in->port,in->owner,in->phase,(unsigned)in->used,errno);in->failed=1;input_close(in);return -1;
}
