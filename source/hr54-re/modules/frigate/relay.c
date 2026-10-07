/* go2rtc MSE/fMP4 -> receiver MPEG-TS. Packet copy only: no codecs opened.
 * Frigate exposes /live/mse/api/ws even when its go2rtc HTTP API is private.
 * stdout is media only; diagnostics never contain upstream bodies or URLs. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <libavformat/avformat.h>
#include <libavutil/base64.h>
#include <libavutil/sha.h>

static int io(int fd,void *buffer,size_t n,int writing){
    unsigned char *p=buffer;
    while(n){ssize_t k=writing?write(fd,p,n):read(fd,p,n);if(k<0&&errno==EINTR)continue;if(k<=0)return -1;p+=k;n-=k;}return 0;
}
static int entropy(void *p,size_t n){int fd=open("/dev/urandom",O_RDONLY);if(fd<0)return -1;int rc=io(fd,p,n,0);close(fd);return rc;}
static int send_frame(int fd,int opcode,const void *data,size_t n){
    if(n>125)return -1;unsigned char b[131];b[0]=0x80|opcode;b[1]=0x80|n;
    if(entropy(b+2,4))return -1;
    for(size_t i=0;i<n;i++)b[i+6]=((const unsigned char *)data)[i]^b[2+i%4];
    return io(fd,b,n+6,1);
}
static int websocket(const char *host,int port,const char *stream){
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_port=htons(port);
    if(inet_pton(AF_INET,host,&addr.sin_addr)!=1)return -1;
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return -1;
    struct timeval timeout={10,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
    if(connect(fd,(struct sockaddr *)&addr,sizeof addr))goto failed;
    unsigned char random[16],digest[20];char key[32],accept[32],encoded[769],request[1400],head[8193];
    if(entropy(random,sizeof random))goto failed;av_base64_encode(key,sizeof key,random,sizeof random);
    struct AVSHA *sha=av_sha_alloc();if(!sha)goto failed;
    av_sha_init(sha,160);av_sha_update(sha,(unsigned char *)key,strlen(key));
    const char *guid="258EAFA5-E914-47DA-95CA-C5AB0DC85B11";av_sha_update(sha,(const unsigned char *)guid,strlen(guid));av_sha_final(sha,digest);av_free(sha);
    av_base64_encode(accept,sizeof accept,digest,sizeof digest);
    size_t used=0;for(const unsigned char *p=(const unsigned char *)stream;*p;p++){
        if((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("-._~",*p))encoded[used++]=*p;
        else{snprintf(encoded+used,4,"%%%02X",*p);used+=3;}
    }encoded[used]=0;
    int n=snprintf(request,sizeof request,"GET /live/mse/api/ws?src=%s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",encoded,host,port,key);
    if(n<0||n>=(int)sizeof request||io(fd,request,n,1))goto failed;
    used=0;while(used<sizeof head-1){if(io(fd,head+used,1,0))goto failed;used++;if(used>=4&&!memcmp(head+used-4,"\r\n\r\n",4))break;}
    head[used]=0;int status=0;sscanf(head,"HTTP/%*s %d",&status);
    if(status!=101){fprintf(stderr,"Frigate WebSocket HTTP status=%d (expected 101)\n",status);goto failed;}
    int valid=0;char *save=NULL;for(char *line=strtok_r(head,"\r\n",&save);line;line=strtok_r(NULL,"\r\n",&save)){
        if(!strncasecmp(line,"Sec-WebSocket-Accept:",21)){char *value=line+21;while(*value==' '||*value=='\t')value++;valid=!strcmp(value,accept);}
    }
    if(!valid)goto failed;
    /* These are go2rtc's protocol codec selector constants, not the camera's
     * profile/level. Do not advertise HEVC, Opus, or raw camera audio. */
    const char *mse="{\"type\":\"mse\",\"value\":\"avc1.640029,mp4a.40.2\"}";
    if(send_frame(fd,1,mse,strlen(mse)))goto failed;
    fprintf(stderr,"Frigate stream endpoint=/live/mse/api/ws format=fMP4 selected=H.264/AAC packet-copy\n");return fd;
failed:close(fd);return -1;
}
static int feed(int fd,int output){
    unsigned char buf[32768];int fragmented=0;
    for(;;){
        unsigned char h[2],ext[8];if(io(fd,h,2,0))return 1;
        int opcode=h[0]&15,fin=h[0]&128;uint64_t size=h[1]&127;
        if(h[0]&0x70||h[1]&128)return 1;
        if(size==126){if(io(fd,ext,2,0))return 1;size=((unsigned)ext[0]<<8)|ext[1];}
        else if(size==127){if(io(fd,ext,8,0))return 1;size=0;for(int i=0;i<8;i++)size=(size<<8)|ext[i];}
        if(size>2*1024*1024||(opcode>=8&&(!fin||size>125)))return 1;
        if(opcode==8)return 0;
        if(opcode==9||opcode==10){if(io(fd,buf,size,0))return 1;if(opcode==9&&send_frame(fd,10,buf,size))return 1;continue;}
        if(opcode==1){
            if(!fin||size>4096||io(fd,buf,size,0))return 1;buf[size]=0;
            /* Only a fixed codec negotiation summary is logged. An error
             * message from go2rtc can include source credentials. */
            if(strstr((char *)buf,"\"error\"")||!strstr((char *)buf,"avc1.")){fprintf(stderr,"Frigate MSE rejected or lacks H.264 video\n");return 1;}
            fprintf(stderr,"Frigate MSE negotiated H.264%s\n",strstr((char *)buf,"mp4a.40.2")?"/AAC":" (video only)");continue;
        }
        if((opcode!=2&&opcode!=0)||(opcode==0&&!fragmented)||(opcode==2&&fragmented))return 1;
        fragmented=!fin;
        while(size){size_t n=size>sizeof buf?sizeof buf:(size_t)size;if(io(fd,buf,n,0)||io(output,buf,n,1))return 1;size-=n;}
    }
}
static int packet_copy(void){
    AVFormatContext *in=NULL,*out=NULL;AVPacket *packet=av_packet_alloc();int rc=-1,map[128],video=0;
    if(!packet)return 1;
    if((rc=avformat_open_input(&in,"pipe:0",NULL,NULL))<0)goto done;
    if(in->nb_streams>128){rc=AVERROR_INVALIDDATA;goto done;}
    if((rc=avformat_alloc_output_context2(&out,NULL,"mpegts","pipe:1"))<0)goto done;
    for(unsigned i=0;i<in->nb_streams;i++){
        AVCodecParameters *params=in->streams[i]->codecpar;map[i]=-1;
        if(params->codec_id!=AV_CODEC_ID_H264&&params->codec_id!=AV_CODEC_ID_AAC)continue;
        if(params->codec_id==AV_CODEC_ID_H264){if(params->width>1920||params->height>1080){fprintf(stderr,"Frigate H.264 dimensions exceed 1920x1080\n");rc=AVERROR_INVALIDDATA;goto done;}video=1;}
        AVStream *s=avformat_new_stream(out,NULL);if(!s){rc=AVERROR(ENOMEM);goto done;}map[i]=s->index;
        if((rc=avcodec_parameters_copy(s->codecpar,params))<0)goto done;s->codecpar->codec_tag=0;s->time_base=in->streams[i]->time_base;
    }
    if(!video){fprintf(stderr,"Frigate fMP4 has no H.264 track\n");rc=AVERROR_INVALIDDATA;goto done;}
    if((rc=avio_open(&out->pb,"pipe:1",AVIO_FLAG_WRITE))<0)goto done;
    out->flags|=AVFMT_FLAG_FLUSH_PACKETS;
    if((rc=avformat_write_header(out,NULL))<0)goto done;
    long packets=0;
    while((rc=av_read_frame(in,packet))>=0){
        int i=packet->stream_index;if(i<0||i>=(int)in->nb_streams||map[i]<0){av_packet_unref(packet);continue;}
        av_packet_rescale_ts(packet,in->streams[i]->time_base,out->streams[map[i]]->time_base);packet->stream_index=map[i];packet->pos=-1;
        rc=av_interleaved_write_frame(out,packet);av_packet_unref(packet);if(rc<0)goto done;
        if(!packets++)fprintf(stderr,"Frigate packet-copy produced MPEG-TS\n");
    }
    if(rc==AVERROR_EOF)rc=av_write_trailer(out);
done:
    if(rc<0){char error[128];av_strerror(rc,error,sizeof error);fprintf(stderr,"Frigate packet-copy failed: %s\n",error);}
    if(out){if(out->pb)avio_closep(&out->pb);avformat_free_context(out);}avformat_close_input(&in);av_packet_free(&packet);return rc<0?1:0;
}
int main(int argc,char **argv){
    if(argc!=4||strlen(argv[3])>255)return 2;char *end;long port=strtol(argv[2],&end,10);if(*end||port<1||port>65535)return 2;
    signal(SIGPIPE,SIG_IGN);int fd=websocket(argv[1],port,argv[3]);if(fd<0){fprintf(stderr,"Frigate WebSocket connection/upgrade failed\n");return 1;}
    int pipefd[2];if(pipe(pipefd)){close(fd);return 1;}pid_t owner=getpid(),child=fork();
    if(child<0){close(fd);close(pipefd[0]);close(pipefd[1]);return 1;}
    if(!child){prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()!=owner)_exit(127);close(pipefd[0]);int rc=feed(fd,pipefd[1]);close(fd);close(pipefd[1]);_exit(rc);}
    close(fd);close(pipefd[1]);dup2(pipefd[0],0);close(pipefd[0]);int rc=packet_copy();kill(child,SIGKILL);int status;while(waitpid(child,&status,0)<0&&errno==EINTR){}return rc;
}
