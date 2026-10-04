#include "dispatcher.h"
#include <signal.h>

#define BROKER_PORT 47953
#define MAX_CLIENTS 4
#define MAX_KEYS 64
#define OUT_CAP 2048

typedef struct {int fd;uint32_t owner;unsigned char in[512],out[OUT_CAP];size_t used,out_used;uint32_t keys[MAX_KEYS];size_t key_count;unsigned char held[MAX_KEYS];} Client;
static Client clients[MAX_CLIENTS];
static volatile sig_atomic_t stopping;
static uint32_t next_owner=0x48520000;
static int tracing(void){const char *s=getenv("HR54_INPUT_TRACE");return s&&strcmp(s,"0");}

static uint32_t get32(const unsigned char *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void set32(unsigned char *p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static void on_signal(int sig){(void)sig;stopping=1;}
static void drop(Client *c){if(c->fd>=0)close(c->fd);memset(c,0,sizeof(*c));c->fd=-1;}
static int queue(Client *c,const unsigned char *p,size_t n){if(n>OUT_CAP-c->out_used)return -1;memcpy(c->out+c->out_used,p,n);c->out_used+=n;return 0;}
static int wants(const Client *c,uint32_t raw){for(size_t i=0;i<c->key_count;i++)if(c->keys[i]==raw)return (int)i+1;return 0;}
static void event(void *unused,KeyEvent ev){(void)unused;uint32_t raw=ev.raw&0xffff;unsigned char msg[12];set32(msg,12);set32(msg+4,8);set32(msg+8,raw|(ev.pressed?0x10000:0));int interested=0;for(int i=0;i<MAX_CLIENTS;i++){Client *c=&clients[i];int at;if(c->fd>=0&&(at=wants(c,raw))){interested++;if(queue(c,msg,sizeof(msg))){fprintf(stderr,"broker-trace: queue failed consumer=%u raw=%08x\n",c->owner,ev.raw);drop(c);continue;}if(tracing())fprintf(stderr,"broker-trace: queued consumer=%u raw=%08x pressed=%d repeat=%d keys=%u bytes=%u\n",c->owner,ev.raw,ev.pressed,ev.repeat,(unsigned)c->key_count,(unsigned)c->out_used);c->held[at-1]=(unsigned char)ev.pressed;}}if(tracing())fprintf(stderr,"broker-trace: raw=%08x interested=%d\n",ev.raw,interested);}
static int flush(Client *c){while(c->out_used){ssize_t n=send(c->fd,c->out,c->out_used,MSG_NOSIGNAL);if(n<0&&(errno==EAGAIN||errno==EINTR))return 0;if(n<=0)return -1;if(tracing())fprintf(stderr,"broker-trace: sent consumer=%u bytes=%u\n",c->owner,(unsigned)n);c->out_used-=n;memmove(c->out,c->out+n,c->out_used);}return 0;}
static int release_held(Client *c){unsigned char msg[12];set32(msg,12);set32(msg+4,8);for(size_t i=0;i<c->key_count;i++)if(c->held[i]){set32(msg+8,c->keys[i]);if(queue(c,msg,sizeof(msg)))return -1;}return 0;}
static int frame(Client *c,const unsigned char *p,size_t n){uint32_t command=get32(p+4);if(command==16)return n==12?0:-1;if(command==4)return -1;if(command!=0||n<24)return -1;if(get32(p+8)!=c->owner||get32(p+12)!=1)return -1;uint32_t shared=get32(p+16),exclusive=get32(p+20);if(!shared||!exclusive)return -1;size_t total=(size_t)(shared-1)+(size_t)(exclusive-1);if(total>MAX_KEYS||24+total*4!=n)return -1;if(release_held(c))return -1;c->key_count=0;memset(c->held,0,sizeof(c->held));for(size_t i=0;i<total;i++){uint32_t raw=get32(p+24+i*4)&0xffff;if(!raw)continue;if(wants(c,raw))return -1;c->keys[c->key_count++]=raw;}unsigned char ack[12];set32(ack,12);set32(ack+4,7);set32(ack+8,1);return queue(c,ack,sizeof(ack));}
static int pump_client(Client *c){if(flush(c))return -1;for(int budget=0;budget<16;budget++){if(c->used>=4){uint32_t n=get32(c->in);if(n<8||n>sizeof(c->in)||n%4)return -1;if(c->used>=n){int rc=frame(c,c->in,n);c->used-=n;memmove(c->in,c->in+n,c->used);if(rc)return -1;continue;}}if(c->used==sizeof(c->in))return -1;ssize_t n=recv(c->fd,c->in+c->used,sizeof(c->in)-c->used,0);if(n<0&&(errno==EAGAIN||errno==EINTR))return 0;if(n<=0)return -1;c->used+=(size_t)n;}return 0;}
static void address(struct sockaddr_in *a){memset(a,0,sizeof(*a));a->sin_family=AF_INET;unsigned char *p=(unsigned char *)&a->sin_port;p[0]=BROKER_PORT>>8;p[1]=BROKER_PORT&255;p=(unsigned char *)&a->sin_addr;p[0]=127;p[3]=1;}
static int listen_socket(void){int fd=socket(AF_INET,SOCK_STREAM,0),one=1;if(fd<0)return -1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));fcntl(fd,F_SETFD,FD_CLOEXEC);int f=fcntl(fd,F_GETFL,0);if(f<0||fcntl(fd,F_SETFL,f|O_NONBLOCK)<0){close(fd);return -1;}struct sockaddr_in a;address(&a);if(bind(fd,(struct sockaddr *)&a,sizeof(a))||listen(fd,MAX_CLIENTS)){close(fd);return -1;}return fd;}
static int check(void){int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return 1;struct sockaddr_in a;address(&a);int rc=connect(fd,(struct sockaddr *)&a,sizeof(a));close(fd);return rc?1:0;}
int main(int argc,char **argv){if(argc==2&&!strcmp(argv[1],"--check"))return check();if(argc!=1)return 2;for(int i=0;i<MAX_CLIENTS;i++)clients[i].fd=-1;signal(SIGINT,on_signal);signal(SIGTERM,on_signal);signal(SIGPIPE,SIG_IGN);int listener=listen_socket();if(listener<0){perror("broker listen");return 1;}Input hardware={.fd=-1};if(input_open(&hardware,2)){close(listener);return 1;}hardware.wanted_active=1;uint64_t deadline=ui_now()+4000;while(!stopping){uint64_t now=ui_now();if(input_pump(&hardware,now,event,NULL)<0){fprintf(stderr,"hr54-input-broker: hardware ownership lost\n");break;}if(!hardware.ready&&now>=deadline)break;for(;;){int fd=accept(listener,NULL,NULL);if(fd<0){if(errno!=EAGAIN&&errno!=EINTR)stopping=1;break;}int slot=-1;for(int i=0;i<MAX_CLIENTS;i++)if(clients[i].fd<0){slot=i;break;}if(slot<0){close(fd);continue;}int f=fcntl(fd,F_GETFL,0);if(f<0||fcntl(fd,F_SETFL,f|O_NONBLOCK)<0){close(fd);continue;}fcntl(fd,F_SETFD,FD_CLOEXEC);Client *c=&clients[slot];memset(c,0,sizeof(*c));c->fd=fd;c->owner=++next_owner;unsigned char hello[4];set32(hello,c->owner);if(queue(c,hello,4))drop(c);}for(int i=0;i<MAX_CLIENTS;i++)if(clients[i].fd>=0&&pump_client(&clients[i]))drop(&clients[i]);struct timespec wait={0,10000000};nanosleep(&wait,NULL);}for(int i=0;i<MAX_CLIENTS;i++)drop(&clients[i]);input_close(&hardware);close(listener);return 1;}
