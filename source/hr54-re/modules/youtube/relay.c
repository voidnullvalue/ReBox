/* Receiver-native dual HTTPS FIFO feeders and packet-copy remux worker. */
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
static void reap(pid_t p){if(p<=1)return;kill(p,SIGKILL);while(waitpid(p,NULL,0)<0&&errno==EINTR){}}
static void parent_guard(pid_t owner){prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()!=owner)_exit(127);}
static const char *fetch_helper,*ca_bundle,*remux;
enum { FETCH_CHUNK=1024*1024,QUEUE_BYTES=2*1024*1024 };
typedef struct {
 const char *url;unsigned char *bytes;size_t head,used,peak;int done,failed;
 unsigned long long total;pthread_mutex_t lock;pthread_cond_t changed;
} Feed;
static unsigned long long content_size(const char *url){
 if(!strstr(url,".googlevideo.com/"))return 0;
 const char *p=strstr(url,"?clen=");if(!p)p=strstr(url,"&clen=");if(!p)return 0;p+=6;
 if(*p<'0'||*p>'9')return 0;char *end;errno=0;unsigned long long n=strtoull(p,&end,10);
 return errno||(*end&&*end!='&')?0:n;
}
static int queue_put(Feed *q,const unsigned char *bytes,size_t count){
 while(count){pthread_mutex_lock(&q->lock);while(q->used==QUEUE_BYTES)pthread_cond_wait(&q->changed,&q->lock);
  size_t tail=(q->head+q->used)%QUEUE_BYTES,n=QUEUE_BYTES-q->used;if(n>QUEUE_BYTES-tail)n=QUEUE_BYTES-tail;if(n>count)n=count;
  memcpy(q->bytes+tail,bytes,n);q->used+=n;if(q->used>q->peak)q->peak=q->used;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);bytes+=n;count-=n;
 }return 0;
}
static int fetch_part(Feed *q,const char *url,unsigned long long expected){
 int pf[2];if(pipe(pf))return -1;pid_t owner=getpid(),helper=fork();
 if(helper<0){close(pf[0]);close(pf[1]);return -1;}
 if(!helper){parent_guard(owner);dup2(pf[1],1);close(pf[0]);close(pf[1]);execl(fetch_helper,"hr54-iptv-fetch",url,ca_bundle,"Mozilla/5.0","","0",(char *)0);_exit(127);}
 close(pf[1]);char ch;int lines=0;size_t header=0;while(lines<3&&header++<16384&&read(pf[0],&ch,1)==1)if(ch=='\n')lines++;
 if(lines!=3){close(pf[0]);reap(helper);return -1;}
 unsigned char buf[32768];ssize_t n;unsigned long long received=0;int failed=0;
 while((n=read(pf[0],buf,sizeof buf))>0){
  if(expected&&(unsigned long long)n>expected-received){failed=1;break;}
  queue_put(q,buf,(size_t)n);received+=(size_t)n;q->total+=(size_t)n;
 }
 if(n<0||failed){close(pf[0]);reap(helper);return -1;}
 close(pf[0]);int status=0;while(waitpid(helper,&status,0)<0&&errno==EINTR){}
 return status||(expected&&received!=expected)?-1:0;
}
static void *produce(void *ctx){
 Feed *q=ctx;unsigned long long size=content_size(q->url);int failed=0;
 if(size){size_t cap=strlen(q->url)+80;char *url=malloc(cap);if(!url)failed=1;
  for(unsigned long long at=0;!failed&&at<size;){unsigned long long length=size-at;if(length>FETCH_CHUNK)length=FETCH_CHUNK;
   int n=snprintf(url,cap,"%s&range=%llu-%llu",q->url,at,at+length-1);
   if(n<0||(size_t)n>=cap||fetch_part(q,url,length))failed=1;else at+=length;
  }free(url);
 }else failed=fetch_part(q,q->url,0)!=0;
 pthread_mutex_lock(&q->lock);q->failed=failed;q->done=1;pthread_cond_broadcast(&q->changed);pthread_mutex_unlock(&q->lock);return NULL;
}
static pid_t fetch(const char *url,const char *fifo,size_t prebuffer){
 pid_t owner=getpid(),p=fork();if(p)return p;parent_guard(owner);
 Feed q={.url=url,.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER};q.bytes=malloc(QUEUE_BYTES);if(!q.bytes)_exit(1);
 pthread_t worker;if(pthread_create(&worker,NULL,produce,&q))_exit(1);
 int out=open(fifo,O_WRONLY);if(out<0)_exit(1);
 pthread_mutex_lock(&q.lock);while(q.used<prebuffer&&!q.done)pthread_cond_wait(&q.changed,&q.lock);pthread_mutex_unlock(&q.lock);
 unsigned char buf[32768];
 for(;;){pthread_mutex_lock(&q.lock);while(!q.used&&!q.done)pthread_cond_wait(&q.changed,&q.lock);
  if(!q.used&&q.done){pthread_mutex_unlock(&q.lock);break;}
  size_t n=q.used;if(n>QUEUE_BYTES-q.head)n=QUEUE_BYTES-q.head;if(n>sizeof buf)n=sizeof buf;
  memcpy(buf,q.bytes+q.head,n);q.head=(q.head+n)%QUEUE_BYTES;q.used-=n;pthread_cond_broadcast(&q.changed);pthread_mutex_unlock(&q.lock);
  size_t at=0;while(at<n){ssize_t w=write(out,buf+at,n-at);if(w<0&&errno==EINTR)continue;if(w<=0)_exit(1);at+=(size_t)w;}
 }
 close(out);pthread_join(worker,NULL);fprintf(stderr,"YouTube HTTPS feeder bytes=%llu peakBuffered=%zu chunked=%d failed=%d\n",q.total,q.peak,content_size(url)!=0,q.failed);free(q.bytes);_exit(q.failed?1:0);
}
int main(int argc,char **argv){
 if(argc!=3)return 2;
 fetch_helper=getenv("REBOX_YOUTUBE_FETCH");if(!fetch_helper)fetch_helper="/var/hr54-persist/jellyfin/bin/hr54-iptv-fetch";
 ca_bundle=getenv("SSL_CERT_FILE");if(!ca_bundle)ca_bundle="/var/hr54-persist/jellyfin/iptv/ca-certificates.crt";
 const char *runtime=getenv("REBOX_YOUTUBE_RUNTIME");if(!runtime)runtime="/var/hr54-persist/jellyfin/youtube";char remux_path[1024];if(snprintf(remux_path,sizeof remux_path,"%s/bin/remux",runtime)>=(int)sizeof remux_path)return 2;remux=remux_path;
char dir[160],v[180],a[180];snprintf(dir,sizeof dir,"/tmp/hr54-youtube-%ld",(long)getpid());if(mkdir(dir,0700))return 1;
 snprintf(v,sizeof v,"%s/video",dir);snprintf(a,sizeof a,"%s/audio",dir);
 pid_t feeders[2]={0,0},mux=0;int status=0,done=0,failed=1;
 if(mkfifo(v,0600)||mkfifo(a,0600))goto cleanup;
 feeders[0]=fetch(argv[1],v,1024*1024);if(feeders[0]<0)goto cleanup;feeders[1]=fetch(argv[2],a,64*1024);if(feeders[1]<0)goto cleanup;
 pid_t owner=getpid();mux=fork();if(mux<0)goto cleanup;
 if(!mux){parent_guard(owner);execl(remux,"remux",v,a,"pipe:1",(char *)0);_exit(127);}
 failed=0;while(!done){pid_t p=waitpid(-1,&status,WNOHANG);if(p<0){if(errno==EINTR)continue;failed=1;break;}if(!p){usleep(100000);continue;}if(p==mux){done=1;failed=status!=0;mux=0;}else{for(int i=0;i<2;i++)if(p==feeders[i])feeders[i]=0;if(status){failed=1;break;}}}
cleanup:
 reap(mux);for(int i=0;i<2;i++)reap(feeders[i]);unlink(v);unlink(a);rmdir(dir);return failed?1:0;
}
