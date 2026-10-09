/* IPTV-owned fragmented MP4 -> MPEG-TS packet copy. stdin/stdout are pipes.
 * No network protocols, decoders, or encoders are linked into this worker. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <libavformat/avformat.h>

int main(void){
 AVFormatContext *in=NULL,*out=NULL;AVPacket *packet=av_packet_alloc();int rc=-1,map[32],video=0;
 signal(SIGPIPE,SIG_DFL);
 if(!packet)return 1;
 if((rc=avformat_open_input(&in,"pipe:0",NULL,NULL))<0)goto done;
 if(in->nb_streams>32){rc=AVERROR_INVALIDDATA;goto done;}
 if((rc=avformat_find_stream_info(in,NULL))<0)goto done;
 if((rc=avformat_alloc_output_context2(&out,NULL,"mpegts","pipe:1"))<0)goto done;
 for(unsigned i=0;i<in->nb_streams;i++){
  AVCodecParameters *par=in->streams[i]->codecpar;map[i]=-1;
  if(par->codec_type==AVMEDIA_TYPE_VIDEO){
   if(par->codec_id!=AV_CODEC_ID_H264){fprintf(stderr,"IPTV remux: unsupported video codec %s\n",avcodec_get_name(par->codec_id));rc=AVERROR_INVALIDDATA;goto done;}
   if(par->width>1920||par->height>1080){fprintf(stderr,"IPTV remux: video dimensions exceed 1920x1080\n");rc=AVERROR_INVALIDDATA;goto done;}video=1;
  }else if(par->codec_type==AVMEDIA_TYPE_AUDIO){
   if(par->codec_id!=AV_CODEC_ID_AAC&&par->codec_id!=AV_CODEC_ID_AC3){fprintf(stderr,"IPTV remux: unsupported audio codec %s\n",avcodec_get_name(par->codec_id));rc=AVERROR_INVALIDDATA;goto done;}
  }else continue;
  AVStream *stream=avformat_new_stream(out,NULL);if(!stream){rc=AVERROR(ENOMEM);goto done;}
  map[i]=stream->index;if((rc=avcodec_parameters_copy(stream->codecpar,par))<0)goto done;
  stream->codecpar->codec_tag=0;stream->time_base=in->streams[i]->time_base;
 }
 if(!video){fprintf(stderr,"IPTV remux: no H.264 video track\n");rc=AVERROR_INVALIDDATA;goto done;}
 if((rc=avio_open(&out->pb,"pipe:1",AVIO_FLAG_WRITE))<0)goto done;
 out->flags|=AVFMT_FLAG_FLUSH_PACKETS;
 if((rc=avformat_write_header(out,NULL))<0)goto done;
 while((rc=av_read_frame(in,packet))>=0){
  int index=packet->stream_index;if(index<0||index>=(int)in->nb_streams||map[index]<0){av_packet_unref(packet);continue;}
  av_packet_rescale_ts(packet,in->streams[index]->time_base,out->streams[map[index]]->time_base);
  packet->stream_index=map[index];packet->pos=-1;
  rc=av_interleaved_write_frame(out,packet);av_packet_unref(packet);if(rc<0)goto done;
 }
 if(rc==AVERROR_EOF)rc=av_write_trailer(out);
done:
 if(rc<0){char error[128];av_strerror(rc,error,sizeof error);fprintf(stderr,"IPTV remux: failed: %s\n",error);}
 if(out){if(out->pb)avio_closep(&out->pb);avformat_free_context(out);}avformat_close_input(&in);av_packet_free(&packet);
 return rc<0?1:0;
}
