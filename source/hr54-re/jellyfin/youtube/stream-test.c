/* One-shot loopback diagnostic server. Fixture feeders execute on receiver.
 * No external network source. Stream mode feeds MP4 FIFOs over ~18s. */
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static pid_t feed(const char *src,const char *fifo){pid_t p=fork();if(p)return p;prctl(PR_SET_PDEATHSIG,SIGKILL);int a=open(src,O_RDONLY),b=open(fifo,O_WRONLY);struct stat st;fstat(a,&st);char buf[8192];ssize_t n;long sent=0;double begin=now();while((n=read(a,buf,sizeof buf))>0){ssize_t off=0;while(off<n){ssize_t w=write(b,buf+off,n-off);if(w<=0)_exit(1);off+=w;}sent+=n;double target=begin+18.0*sent/st.st_size;while(now()<target)usleep(10000);}fprintf(stderr,"feeder %s completed %.3fs bytes=%ld\n",src,now()-begin,sent);close(a);close(b);_exit(0);}
static pid_t fetch(const char *url,const char *fifo){pid_t p=fork();if(p)return p;prctl(PR_SET_PDEATHSIG,SIGKILL);int pipefd[2];if(pipe(pipefd))_exit(1);pid_t helper=fork();if(!helper){prctl(PR_SET_PDEATHSIG,SIGKILL);dup2(pipefd[1],1);close(pipefd[0]);close(pipefd[1]);execl("/var/hr54-persist/jellyfin/bin/hr54-iptv-fetch","hr54-iptv-fetch",url,"/var/hr54-persist/jellyfin/iptv/ca-certificates.crt","Mozilla/5.0","","40",(char *)0);_exit(127);}close(pipefd[1]);char ch;int lines=0;while(lines<3&&read(pipefd[0],&ch,1)==1)if(ch=='\n')lines++;if(lines!=3){kill(helper,SIGKILL);waitpid(helper,NULL,0);_exit(1);}int b=open(fifo,O_WRONLY);char buf[32768];ssize_t n;long bytes=0;while((n=read(pipefd[0],buf,sizeof buf))>0){ssize_t at=0;while(at<n){ssize_t w=write(b,buf+at,n-at);if(w<=0){kill(helper,SIGKILL);waitpid(helper,NULL,0);_exit(1);}at+=w;}bytes+=n;}close(b);close(pipefd[0]);int status;waitpid(helper,&status,0);fprintf(stderr,"HTTPS feeder bytes=%ld status=%d\n",bytes,status);_exit(status?1:0);}
int main(int argc,char **argv){if(argc!=2&&argc!=4)return 2;alarm(50);int s=socket(AF_INET,SOCK_STREAM,0),yes=1;setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(18100),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};if(bind(s,(void *)&a,sizeof a)||listen(s,1))return 1;fprintf(stderr,"ready loopback:18100\n");int c=accept(s,NULL,NULL);close(s);if(c<0)return 1;char req[2048];if(read(c,req,sizeof req)<=0)return 1;
 const char *video="/var/hr54-transfer/yt-video.mp4",*audio="/var/hr54-transfer/yt-audio.m4a";pid_t feeders[2]={0};if(atoi(argv[1])){unlink("/tmp/yt-video-fifo");unlink("/tmp/yt-audio-fifo");mkfifo("/tmp/yt-video-fifo",0600);mkfifo("/tmp/yt-audio-fifo",0600);if(atoi(argv[1])==2&&argc==4){feeders[0]=fetch(argv[2],"/tmp/yt-video-fifo");feeders[1]=fetch(argv[3],"/tmp/yt-audio-fifo");}else{feeders[0]=feed(video,"/tmp/yt-video-fifo");feeders[1]=feed(audio,"/tmp/yt-audio-fifo");}video="/tmp/yt-video-fifo";audio="/tmp/yt-audio-fifo";}
 const char header[]="HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nConnection: close\r\n\r\n";if(write(c,header,sizeof header-1)!=sizeof header-1)return 1;
 pid_t child=fork();if(!child){prctl(PR_SET_PDEATHSIG,SIGKILL);dup2(c,1);close(c);execl("/var/hr54-transfer/yt-remux","yt-remux",video,audio,"pipe:1",(char *)0);_exit(127);}close(c);int status;waitpid(child,&status,0);for(int i=0;i<2;i++)if(feeders[i]){kill(feeders[i],SIGTERM);waitpid(feeders[i],NULL,0);}unlink("/tmp/yt-video-fifo");unlink("/tmp/yt-audio-fifo");fprintf(stderr,"remux status=%d\n",status);return status?1:0;}
