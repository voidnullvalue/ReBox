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
static void reap(pid_t p){if(p<=1)return;kill(p,SIGKILL);while(waitpid(p,NULL,0)<0&&errno==EINTR){}}
static void parent_guard(pid_t owner){prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()!=owner)_exit(127);}
static const char *fetch_helper,*ca_bundle,*remux;
static pid_t fetch(const char *url,const char *fifo){
 pid_t owner=getpid(),p=fork();if(p)return p;parent_guard(owner);
 int pf[2];if(pipe(pf))_exit(1);owner=getpid();pid_t helper=fork();
 if(helper<0){close(pf[0]);close(pf[1]);_exit(1);}
 if(!helper){parent_guard(owner);dup2(pf[1],1);close(pf[0]);close(pf[1]);execl(fetch_helper,"hr54-iptv-fetch",url,ca_bundle,"Mozilla/5.0","","0",(char *)0);_exit(127);}
 close(pf[1]);char ch;int lines=0;while(lines<3&&read(pf[0],&ch,1)==1)if(ch=='\n')lines++;
 if(lines!=3){reap(helper);_exit(1);}int b=open(fifo,O_WRONLY);if(b<0){reap(helper);_exit(1);}
 char buf[32768];ssize_t n;long bytes=0;
 while((n=read(pf[0],buf,sizeof buf))>0){ssize_t at=0;while(at<n){ssize_t w=write(b,buf+at,n-at);if(w<0&&errno==EINTR)continue;if(w<=0){reap(helper);_exit(1);}at+=w;}bytes+=n;}
 close(b);close(pf[0]);int status=0;while(waitpid(helper,&status,0)<0&&errno==EINTR){}
 fprintf(stderr,"YouTube HTTPS feeder bytes=%ld exit=%d signal=%d\n",bytes,WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0);_exit(n<0||status?1:0);
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
 feeders[0]=fetch(argv[1],v);if(feeders[0]<0)goto cleanup;feeders[1]=fetch(argv[2],a);if(feeders[1]<0)goto cleanup;
 pid_t owner=getpid();mux=fork();if(mux<0)goto cleanup;
 if(!mux){parent_guard(owner);execl(remux,"remux",v,a,"pipe:1",(char *)0);_exit(127);}
 failed=0;while(!done){pid_t p=waitpid(-1,&status,WNOHANG);if(p<0){if(errno==EINTR)continue;failed=1;break;}if(!p){usleep(100000);continue;}if(p==mux){done=1;failed=status!=0;mux=0;}else{for(int i=0;i<2;i++)if(p==feeders[i])feeders[i]=0;if(status){failed=1;break;}}}
cleanup:
 reap(mux);for(int i=0;i<2;i++)reap(feeders[i]);unlink(v);unlink(a);rmdir(dir);return failed?1:0;
}
