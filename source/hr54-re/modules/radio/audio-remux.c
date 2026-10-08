/* Normalize radio audio to stereo AC-3 with an integer encoder plus a looped black AVC track.
 * The HR54 TV pipeline configures AAC even for MP3 TS; packet-copy MP3 is silent. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
static AVFormatContext *output;
static AVCodecContext *encoder;
static AVPacket *black[64];
static int black_count;
static int64_t video_frame,audio_samples,origin=AV_NOPTS_VALUE;
static void checked_at(int r,int line){if(r<0){char e[256];av_strerror(r,e,sizeof e);fprintf(stderr,"radio normalize line %d: %s\n",line,e);exit(1);}}
#define checked(r) checked_at((r),__LINE__)
static double begun,last_report,decode_time,resample_time,encode_time,mux_time;
static int live;static long long input_bytes;static double audio_end;
static double wall(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void report(int force){double now=wall();if(!force&&now-last_report<10)return;struct rusage usage;getrusage(RUSAGE_SELF,&usage);
 fprintf(stderr,"RADIO_METRICS wall=%.3f audio=%.3f input=%lld output=%lld cpu=%.3f decode=%.3f resample=%.3f encode=%.3f mux=%.3f video=%lld\n",now-begun,audio_end,input_bytes,(long long)avio_tell(output->pb),usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1e6+usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1e6,decode_time,resample_time,encode_time,mux_time,(long long)video_frame);last_report=now;
}
static void mux_audio(AVPacket *p,AVRational tb){if(origin==AV_NOPTS_VALUE)origin=p->dts;p->pts-=origin;p->dts-=origin;
 double position=p->dts*av_q2d(tb);audio_end=position+p->duration*av_q2d(tb);
 if(live)while(position>wall()-begun+1){struct timespec pause={0,10000000};nanosleep(&pause,NULL);}
 int64_t audio_time=av_rescale_q(p->dts,tb,(AVRational){1,90000});double start=wall();
 while(video_frame*3600<=audio_time){AVPacket *v=av_packet_clone(black[video_frame%black_count]);if(!v)exit(1);v->stream_index=0;v->pts=v->dts=video_frame*3600;v->duration=3600;v->pos=-1;checked(av_interleaved_write_frame(output,v));av_packet_free(&v);video_frame++;}
 av_packet_rescale_ts(p,tb,output->streams[1]->time_base);p->stream_index=1;p->pos=-1;checked(av_interleaved_write_frame(output,p));mux_time+=wall()-start;report(0);
}
static void encoded(void){AVPacket *p=av_packet_alloc();if(!p)exit(1);int rc;
 for(;;){double start=wall();rc=avcodec_receive_packet(encoder,p);encode_time+=wall()-start;if(rc<0)break;mux_audio(p,encoder->time_base);av_packet_unref(p);}
 if(rc!=AVERROR(EAGAIN)&&rc!=AVERROR_EOF)checked(rc);av_packet_free(&p);
}
static void encode_fifo(AVAudioFifo *fifo,int flush){while(av_audio_fifo_size(fifo)>=encoder->frame_size||(flush&&av_audio_fifo_size(fifo)>0)){
 AVFrame *f=av_frame_alloc();if(!f)exit(1);f->format=encoder->sample_fmt;f->sample_rate=encoder->sample_rate;checked(av_channel_layout_copy(&f->ch_layout,&encoder->ch_layout));f->nb_samples=encoder->frame_size;checked(av_frame_get_buffer(f,0));
 int count=av_audio_fifo_size(fifo);if(count>f->nb_samples)count=f->nb_samples;checked(av_audio_fifo_read(fifo,(void **)f->data,count));
 if(count<f->nb_samples)av_samples_set_silence(f->data,count,f->nb_samples-count,2,encoder->sample_fmt);
 f->pts=audio_samples;audio_samples+=f->nb_samples;double start=wall();checked(avcodec_send_frame(encoder,f));encode_time+=wall()-start;av_frame_free(&f);encoded();}}
static void decode(AVCodecContext *decoder,SwrContext **swr_ref,AVAudioFifo *fifo){AVFrame *f=av_frame_alloc();if(!f)exit(1);int rc;
 for(;;){double start=wall();rc=avcodec_receive_frame(decoder,f);decode_time+=wall()-start;if(rc<0)break;
 if(!*swr_ref){checked(swr_alloc_set_opts2(swr_ref,&encoder->ch_layout,encoder->sample_fmt,encoder->sample_rate,&f->ch_layout,f->format,f->sample_rate,0,NULL));checked(swr_init(*swr_ref));}
 SwrContext *swr=*swr_ref;int capacity=swr_get_out_samples(swr,f->nb_samples);uint8_t **data=NULL;int linesize;checked(av_samples_alloc_array_and_samples(&data,&linesize,2,capacity,AV_SAMPLE_FMT_S32P,0));
 double start_resample=wall();int count=swr_convert(swr,data,capacity,(const uint8_t **)f->extended_data,f->nb_samples);resample_time+=wall()-start_resample;checked(count);if(count){checked(av_audio_fifo_realloc(fifo,av_audio_fifo_size(fifo)+count));checked(av_audio_fifo_write(fifo,(void **)data,count));}av_freep(&data[0]);av_freep(&data);av_frame_unref(f);encode_fifo(fifo,0);}
 if(rc!=AVERROR(EAGAIN)&&rc!=AVERROR_EOF)checked(rc);av_frame_free(&f);
}
static void copy_aac(AVFormatContext *input,AVFormatContext *video,int vi,int ai,AVPacket *p){
 checked(avformat_alloc_output_context2(&output,NULL,"mpegts","pipe:1"));AVStream *vs=avformat_new_stream(output,NULL),*as=avformat_new_stream(output,NULL);if(!vs||!as)exit(1);
 checked(avcodec_parameters_copy(vs->codecpar,video->streams[vi]->codecpar));vs->codecpar->codec_tag=0;vs->time_base=(AVRational){1,90000};checked(avcodec_parameters_copy(as->codecpar,input->streams[ai]->codecpar));as->codecpar->codec_tag=0;as->time_base=input->streams[ai]->time_base;
 checked(avio_open(&output->pb,"pipe:1",AVIO_FLAG_WRITE));output->flags|=AVFMT_FLAG_FLUSH_PACKETS;checked(avformat_write_header(output,NULL));int rc;int64_t next=0;
 while((rc=av_read_frame(input,p))>=0){if(p->stream_index==ai){input_bytes+=p->size;AVRational clock={1,input->streams[ai]->codecpar->sample_rate};int64_t duration=p->duration>0?av_rescale_q(p->duration,input->streams[ai]->time_base,clock):1024;p->pts=p->dts=next;p->duration=duration;next+=duration;mux_audio(p,clock);}av_packet_unref(p);}if(rc!=AVERROR_EOF)checked(rc);checked(av_write_trailer(output));report(1);
 avio_closep(&output->pb);avformat_free_context(output);avformat_close_input(&input);avformat_close_input(&video);av_packet_free(&p);for(int i=0;i<black_count;i++)av_packet_free(&black[i]);
}
int main(void){begun=last_report=wall();live=getenv("REBOX_RADIO_LIVE")!=NULL;AVFormatContext *input=NULL,*video=NULL;AVPacket *p=av_packet_alloc();if(!p)return 1;const char *path=getenv("REBOX_RADIO_VIDEO");if(!path)return 2;
 checked(avformat_open_input(&video,path,NULL,NULL));checked(avformat_find_stream_info(video,NULL));int vi=-1;for(unsigned i=0;i<video->nb_streams;i++)if(video->streams[i]->codecpar->codec_id==AV_CODEC_ID_H264){vi=i;break;}if(vi<0)return 1;
 while(av_read_frame(video,p)>=0){if(p->stream_index==vi){if(black_count==64)return 1;black[black_count++]=av_packet_clone(p);}av_packet_unref(p);}if(!black_count)return 1;
 AVDictionary *options=NULL;av_dict_set(&options,"probesize","65536",0);av_dict_set(&options,"analyzeduration","500000",0);checked(avformat_open_input(&input,"pipe:0",NULL,&options));av_dict_free(&options);checked(avformat_find_stream_info(input,NULL));
 int ai=av_find_best_stream(input,AVMEDIA_TYPE_AUDIO,-1,-1,NULL,0);checked(ai);AVCodecParameters *par=input->streams[ai]->codecpar;if(par->codec_id==AV_CODEC_ID_AAC&&par->profile==AV_PROFILE_AAC_LOW&&par->ch_layout.nb_channels>0&&par->ch_layout.nb_channels<=2&&(par->sample_rate==32000||par->sample_rate==44100||par->sample_rate==48000)){fprintf(stderr,"RADIO_MODE copy-aac-lc\n");copy_aac(input,video,vi,ai,p);return 0;}
 fprintf(stderr,"RADIO_MODE convert-ac3-fixed source_codec=%d\n",par->codec_id);const AVCodec *codec=avcodec_find_decoder(par->codec_id);if(!codec)return 1;
 AVCodecContext *decoder=avcodec_alloc_context3(codec);checked(avcodec_parameters_to_context(decoder,par));checked(avcodec_open2(decoder,codec,NULL));
 encoder=avcodec_alloc_context3(avcodec_find_encoder_by_name("ac3_fixed"));if(!encoder)return 1;encoder->sample_fmt=AV_SAMPLE_FMT_S32P;encoder->sample_rate=par->sample_rate;if(encoder->sample_rate!=32000&&encoder->sample_rate!=44100&&encoder->sample_rate!=48000)encoder->sample_rate=48000;encoder->time_base=(AVRational){1,encoder->sample_rate};encoder->bit_rate=192000;av_channel_layout_default(&encoder->ch_layout,2);checked(avcodec_open2(encoder,encoder->codec,NULL));
 SwrContext *swr=NULL;AVAudioFifo *fifo=av_audio_fifo_alloc(encoder->sample_fmt,2,encoder->frame_size);if(!fifo)return 1;
 checked(avformat_alloc_output_context2(&output,NULL,"mpegts","pipe:1"));AVStream *vs=avformat_new_stream(output,NULL),*as=avformat_new_stream(output,NULL);if(!vs||!as)return 1;checked(avcodec_parameters_copy(vs->codecpar,video->streams[vi]->codecpar));vs->codecpar->codec_tag=0;vs->time_base=(AVRational){1,90000};checked(avcodec_parameters_from_context(as->codecpar,encoder));as->time_base=encoder->time_base;
 checked(avio_open(&output->pb,"pipe:1",AVIO_FLAG_WRITE));output->flags|=AVFMT_FLAG_FLUSH_PACKETS;checked(avformat_write_header(output,NULL));int rc;
 while((rc=av_read_frame(input,p))>=0){if(p->stream_index==ai){input_bytes+=p->size;double start=wall();checked(avcodec_send_packet(decoder,p));decode_time+=wall()-start;decode(decoder,&swr,fifo);}av_packet_unref(p);}if(rc!=AVERROR_EOF)checked(rc);
 checked(avcodec_send_packet(decoder,NULL));decode(decoder,&swr,fifo);encode_fifo(fifo,1);checked(avcodec_send_frame(encoder,NULL));encoded();checked(av_write_trailer(output));report(1);
 av_audio_fifo_free(fifo);swr_free(&swr);avcodec_free_context(&decoder);avcodec_free_context(&encoder);avio_closep(&output->pb);avformat_free_context(output);avformat_close_input(&input);avformat_close_input(&video);av_packet_free(&p);for(int i=0;i<black_count;i++)av_packet_free(&black[i]);return 0;}
