/* Bounded two-track packet copier. No decoder/encoder is opened.
 * Inputs must expose AVC/AAC in MP4; stdout/file/pipe MPEG-TS output.
 * FFmpeg MOV demuxer and MPEG-TS muxer handle framing and PAT/PMT/PCR. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
static void checked(int r,const char *where){if(r<0){char err[256];av_strerror(r,err,sizeof err);fprintf(stderr,"%s: %s\n",where,err);exit(1);}}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char **argv){
 if(argc!=4){fprintf(stderr,"usage: remux video.mp4 audio.m4a output.ts\n");return 2;}
 double start=now(),first=0;AVFormatContext *in[2]={0},*out=0;AVPacket *pkt[2]={av_packet_alloc(),av_packet_alloc()};int idx[2]={-1,-1},ready[2]={0},eof[2]={0};long long bytes=0,packets=0;
 for(int i=0;i<2;i++){
 checked(avformat_open_input(&in[i],argv[i+1],NULL,NULL),"open MP4");
 for(unsigned j=0;j<in[i]->nb_streams;j++)if(in[i]->streams[j]->codecpar->codec_id==(i?AV_CODEC_ID_AAC:AV_CODEC_ID_H264)){idx[i]=j;break;}
 if(idx[i]<0){fprintf(stderr,"required AVC/AAC track absent\n");return 1;}
 }
 checked(avformat_alloc_output_context2(&out,NULL,"mpegts",argv[3]),"create TS");
 for(int i=0;i<2;i++){AVStream *s=avformat_new_stream(out,NULL);if(!s)return 1;checked(avcodec_parameters_copy(s->codecpar,in[i]->streams[idx[i]]->codecpar),"copy parameters");s->codecpar->codec_tag=0;s->time_base=in[i]->streams[idx[i]]->time_base;}
 checked(avio_open(&out->pb,argv[3],AVIO_FLAG_WRITE),"open output");out->flags|=AVFMT_FLAG_FLUSH_PACKETS;
 checked(avformat_write_header(out,NULL),"TS header");
 while(!eof[0]||!eof[1]){
 for(int i=0;i<2;i++)while(!ready[i]&&!eof[i]){int r=av_read_frame(in[i],pkt[i]);if(r==AVERROR_EOF){eof[i]=1;break;}checked(r,"read packet");if(pkt[i]->stream_index!=idx[i]){av_packet_unref(pkt[i]);continue;}ready[i]=1;}
 int i=ready[0]?0:1;if(!ready[i])break;
 if(ready[0]&&ready[1]){int64_t a=pkt[0]->dts==AV_NOPTS_VALUE?pkt[0]->pts:pkt[0]->dts,b=pkt[1]->dts==AV_NOPTS_VALUE?pkt[1]->pts:pkt[1]->dts;if(av_compare_ts(a,in[0]->streams[idx[0]]->time_base,b,in[1]->streams[idx[1]]->time_base)>0)i=1;}
 bytes+=pkt[i]->size;packets++;av_packet_rescale_ts(pkt[i],in[i]->streams[idx[i]]->time_base,out->streams[i]->time_base);pkt[i]->stream_index=i;pkt[i]->pos=-1;
 checked(av_interleaved_write_frame(out,pkt[i]),"write TS packet");if(!first){first=now()-start;fprintf(stderr,"first_packet_seconds=%.3f\n",first);}ready[i]=0;
 }
 checked(av_write_trailer(out),"TS trailer");avio_closep(&out->pb);avformat_free_context(out);for(int i=0;i<2;i++){av_packet_free(&pkt[i]);avformat_close_input(&in[i]);}
 struct rusage r;getrusage(RUSAGE_SELF,&r);fprintf(stderr,"elapsed=%.3f cpu=%.3f maxrss_kib=%ld packets=%lld input_packet_bytes=%lld\n",now()-start,r.ru_utime.tv_sec+r.ru_utime.tv_usec/1e6+r.ru_stime.tv_sec+r.ru_stime.tv_usec/1e6,r.ru_maxrss,packets,bytes);return 0;
}
