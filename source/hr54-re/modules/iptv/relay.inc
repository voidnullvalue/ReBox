/* One receiver-native IPTV session. Networking uses installed TLS adapter;
 * no video is written to disk and no codec processing/transcoding occurs. */
struct iptv_fetch {int fd;pid_t pid;char url[IPTV_URL_CAP+1],ct[257];};
static int iptv_active(const char *token){int ok;state_lock();ok=S->iptv_active&&!strcmp(S->iptv_token,token);state_unlock();return ok;}
static void iptv_fetch_close(struct iptv_fetch *f){if(f->fd>=0)close(f->fd);if(f->pid>0){int status=0;pid_t got;do{got=waitpid(f->pid,&status,WNOHANG);}while(got<0&&errno==EINTR);if(!got){kill(f->pid,SIGKILL);while(waitpid(f->pid,&status,0)<0&&errno==EINTR){}}jf_log("IPTV fetch exit=%d signal=%d",got>=0&&WIFEXITED(status)?WEXITSTATUS(status):-1,got>=0&&WIFSIGNALED(status)?WTERMSIG(status):0);}f->fd=-1;f->pid=0;}
static ssize_t iptv_read(struct iptv_fetch *f,void *buf,size_t cap,const char *token){
 double until=mono_now()+20;for(;;){if(token&&!iptv_active(token))return fail("IPTV stopped");struct pollfd p={f->fd,POLLIN,0};int r=poll(&p,1,100);if(r<0&&errno==EINTR)continue;if(r<0)return -1;if(r){ssize_t n=read(f->fd,buf,cap);if(n<0&&errno==EINTR)continue;return n;}if(mono_now()>until)return fail("Channel unreachable or TLS connection failed");}}
static int iptv_open(struct iptv_fetch *f,const char *url,const char *ua,const char *ref,int timeout,const char *token){
 memset(f,0,sizeof *f);f->fd=-1;if(strlen(url)>IPTV_URL_CAP||(strncmp(url,"http://",7)&&strncmp(url,"https://",8))||strpbrk(url,"\r\n"))return fail("Unsupported IPTV protocol");
 char helper[512],ca[512],seconds[16];snprintf(helper,sizeof helper,"%s/bin/hr54-iptv-fetch",persist_root);
#ifdef REBOX_IPTV_MODULE
 snprintf(helper,sizeof helper,"%s/bin/fetch",package_root);
#endif
 const char *env=getenv("IPTV_FETCH_CMD");if(env&&*env)snprintf(helper,sizeof helper,"%s",env);snprintf(ca,sizeof ca,"%s/iptv/ca-certificates.crt",persist_root);snprintf(seconds,sizeof seconds,"%d",timeout);
 jf_log("IPTV fetch path=%s timeout=%d userAgent=%s referer=%s",helper,timeout,*ua?"provided":"default",*ref?"provided":"absent");
 pid_t owner=getpid();int p[2];if(pipe(p))return fail("Unable to start TLS helper");f->pid=fork();if(f->pid<0){close(p[0]);close(p[1]);return fail("Unable to start TLS helper");}
 if(!f->pid){prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()!=owner)_exit(127);alarm(0);dup2(p[1],1);close(p[0]);close(p[1]);for(int inherited=3;inherited<1024;inherited++)close(inherited);execl(helper,helper,url,ca,ua,ref,seconds,(char *)NULL);_exit(127);}close(p[1]);f->fd=p[0];
 char header[IPTV_URL_CAP+260];size_t n=0;int newlines=0;while(n<sizeof header-1){char c;ssize_t r=iptv_read(f,&c,1,token);if(r<=0){iptv_fetch_close(f);return fail("Channel unavailable or TLS connection failed (see fetch log)");}header[n++]=c;if(c=='\n'&&++newlines==3)break;}header[n]=0;
 char *ct=strchr(header,'\n');if(!ct||n>=sizeof header-1){iptv_fetch_close(f);return fail("Invalid upstream response");}*ct++=0;
 if(!strncmp(header,"!HTTP:",6)){int code=atoi(header+6);iptv_fetch_close(f);
  if(code==403)return fail("Stream blocked by provider (HTTP 403)");
  if(code==404)return fail("Channel unavailable (HTTP 404)");
  if(code==429)return fail("Provider rate limited stream (HTTP 429)");
  if(code>=500&&code<600)return fail("Provider error (HTTP %d)",code);
  return fail("Channel unavailable (HTTP %d)",code);
 }
 if(!strncmp(header,"!CURL:",6)){int code=atoi(header+6);iptv_fetch_close(f);
  if(code==6)return fail("Channel DNS lookup failed");if(code==7)return fail("Channel connection failed");
  if(code==28)return fail("Channel connection timed out");if(code==60)return fail("Channel TLS certificate validation failed");
  if(code==35)return fail("Channel TLS negotiation failed");return fail("Channel network error (curl %d)",code);
 }
 char *end=strchr(ct,'\n');if(end)*end=0;if(strlen(header)>IPTV_URL_CAP||strlen(ct)>256){iptv_fetch_close(f);return fail("Upstream headers exceed bounds");}snprintf(f->url,sizeof f->url,"%s",header);snprintf(f->ct,sizeof f->ct,"%s",ct);return 0;
}
/* Relative references, root paths, query-only URIs and dot segments. */
static int iptv_resolve(char *out,size_t cap,const char *base,const char *ref){
 if(!*ref||strlen(ref)>IPTV_URL_CAP||strpbrk(ref,"\r\n"))return fail("Invalid HLS URI");
 if(!strncmp(base,"https://",8)&&!strncmp(ref,"http://",7))return fail("HTTPS HLS child downgraded to HTTP");
 if(strstr(ref,"://")){if(strncmp(ref,"http://",7)&&strncmp(ref,"https://",8))return fail("Unsupported HLS URI protocol");if(strlen(ref)>=cap)return -1;strcpy(out,ref);return 0;}
 const char *scheme=strstr(base,"://");if(!scheme)return -1;const char *path=strchr(scheme+3,'/');size_t origin=path?(size_t)(path-base):strlen(base);
 struct sb b={0};if(!strncmp(ref,"//",2)){sb_putn(&b,base,(size_t)(scheme-base)+1);sb_puts(&b,ref);}else{sb_putn(&b,base,origin);if(*ref=='/')sb_puts(&b,ref);else{const char *end=path?path+strcspn(path,"?#"):base+origin;if(*ref=='?'){if(path)sb_putn(&b,path,(size_t)(end-path));else sb_puts(&b,"/");}else{const char *slash=end;while(slash>base+origin&&slash[-1]!='/')slash--;if(path&&slash>path)sb_putn(&b,path,(size_t)(slash-path));else sb_puts(&b,"/");}sb_puts(&b,ref);}}
 if(b.len>=cap){free(b.p);return fail("HLS URL too long");}
 /* Normalize only path; preserve query verbatim. */
 char *p=strstr(b.p,"://");p=p?strchr(p+3,'/'):NULL;if(p){char *q=strpbrk(p,"?#");char *suffix=q?strdup(q):strdup("");if(q)*q=0;char *norm=calloc(strlen(p)+2,1);if(!norm||!suffix){free(norm);free(suffix);free(b.p);return fail("out of memory");}size_t used=0;char *save=NULL;char *part=strtok_r(p,"/",&save);while(part){if(!strcmp(part,"..")){while(used&&norm[used-1]!='/')used--;if(used)used--;norm[used]=0;}else if(strcmp(part,".")){norm[used++]='/';size_t k=strlen(part);memcpy(norm+used,part,k);used+=k;norm[used]=0;}part=strtok_r(NULL,"/",&save);}if(!used)norm[used++]='/';norm[used]=0;size_t prefix=(size_t)(p-b.p);memcpy(out,b.p,prefix);out[prefix]=0;if(prefix+used+strlen(suffix)>=cap){free(norm);free(suffix);free(b.p);return -1;}strcat(out,norm);strcat(out,suffix);free(norm);free(suffix);}else strcpy(out,b.p);free(b.p);return 0;
}
static int iptv_ts(const unsigned char *p,size_t n){return n>=376&&p[0]==0x47&&p[188]==0x47;}
/* PSI sections may span many TS packets, especially PMTs with descriptors.
 * Bound the assembler to MPEG-TS's 1021-byte section limit. */
static int iptv_program(const unsigned char *p,size_t n,unsigned *video,unsigned *audio){
 unsigned char section[1024];size_t used=0,want=0;unsigned pid=8192;int cc=-1,unsupported_audio=0;
 *video=*audio=8192;
 for(size_t at=0;at+188<=n;at+=188){const unsigned char *t=p+at;if(t[0]!=0x47||t[1]&0x80)continue;
  unsigned current=((t[1]&31)<<8)|t[2],af=(t[3]>>4)&3;if(!(af&1))continue;
  size_t off=4;if(af&2){off+=1+t[4];if(off>=188)continue;}
  if(t[1]&0x40){if(off>=188)continue;size_t pointer=t[off++];if(off+pointer>=188)continue;off+=pointer;
   if(t[off]!=2){if(current==pid)used=0;continue;}pid=current;used=want=0;cc=t[3]&15;
  }else if(current!=pid||!used)continue;
  else{if(((cc+1)&15)!=(t[3]&15)){used=0;continue;}cc=t[3]&15;}
  size_t len=188-off;if(len>sizeof section-used)len=sizeof section-used;memcpy(section+used,t+off,len);used+=len;
  if(used>=3&&!want){want=3+(((section[1]&15)<<8)|section[2]);if(want<16||want>sizeof section){used=0;continue;}}
  if(want&&used>=want){size_t es=12+(((section[10]&15)<<8)|section[11]);if(es>want-4){used=0;continue;}
   while(es+5<=want-4){unsigned type=section[es],stream_pid=((section[es+1]&31)<<8)|section[es+2];
    if(type==0x1b)*video=stream_pid;if(*audio==8192&&(type==0x0f||type==0x81))*audio=stream_pid;
    if(type==0x03||type==0x04||type==0x11||type==0x87)unsupported_audio=1;
    es+=5+(((section[es+3]&15)<<8)|section[es+4]);}
   if(*video!=8192){if(*audio==8192&&unsupported_audio)return fail("Unsupported audio codec in MPEG-TS program map");return 0;}
   return fail("Unsupported video codec in MPEG-TS program map");
  }
 }
 return fail("No complete MPEG-TS program map in startup data");
}
static int iptv_h264(const unsigned char *p,size_t n){unsigned video,audio;return iptv_program(p,n,&video,&audio);}
/* Stock playURL probes only the start of TS. Some HLS encoders place the
 * first AAC PES behind a >1 MiB video access unit. Move that complete PES
 * before the first video PES, preserving every packet and per-PID order.
 * This runs once on the already bounded first segment; no codec processing. */
static int iptv_audio_startup(unsigned char *p,size_t n){
 if(n%188)return 0;
 unsigned video=8192,audio=8192;iptv_program(p,n,&video,&audio);
 if(audio==8192||video==8192)return 0;
 size_t insert=n,begin=n,end=n,count=0;
 for(size_t at=0;at+188<=n;at+=188){unsigned char *t=p+at;unsigned pid=((t[1]&31)<<8)|t[2];if(pid==video&&(t[1]&64)&&insert==n)insert=at;if(pid!=audio)continue;if(t[1]&64){if(begin!=n){end=at;break;}begin=at;}if(begin!=n){if((t[1]&128)||(t[3]&192))return 0;count++;if(count>256)return 0;}}
 if(begin==n||end==n||insert>=begin||!count)return 0;
 size_t bytes=count*188;unsigned char *saved=malloc(bytes);if(!saved)return 0;size_t used=0;
 for(size_t at=begin;at<end;at+=188)if((((p[at+1]&31)<<8)|p[at+2])==audio){memcpy(saved+used,p+at,188);used+=188;}
 size_t dst=begin;
 for(size_t at=begin;at+188<=n;at+=188){if(at<end&&((((p[at+1]&31)<<8)|p[at+2])==audio))continue;memmove(p+dst,p+at,188);dst+=188;}
 memmove(p+insert+bytes,p+insert,n-bytes-insert);memcpy(p+insert,saved,bytes);free(saved);
 jf_log("IPTV startup audio PID %u: moved %zu TS packets from byte %zu to %zu for playURL detection",audio,count,begin,insert);return 1;
}

static int iptv_hls(const char *p,size_t n){return n>=7&&!memcmp(p,"#EXTM3U",7);}
static int iptv_download(const char *url,const char *ua,const char *ref,const char *token,struct sb *body,char *effective,int probe){
 struct iptv_fetch f;if(iptv_open(&f,url,ua,ref,probe?8:20,token))return -1;snprintf(effective,IPTV_URL_CAP+1,"%s",f.url);size_t cap=probe?1024*1024:12*1024*1024;int rc=0;char buf[32768];for(;;){ssize_t n=iptv_read(&f,buf,sizeof buf,token);if(n<0){rc=-1;break;}if(!n)break;if(body->len+(size_t)n>cap){rc=fail("Upstream playlist/segment exceeds bounds");break;}if(sb_putn(body,buf,(size_t)n)){rc=-1;break;}if(probe&&body->len>=256*1024&&!iptv_hls(body->p,body->len))break;}
 if(!rc&&!body->len)rc=fail("Channel returned no data");iptv_fetch_close(&f);return rc;
}
struct iptv_segment {unsigned long long seq;char *uri,*map;int discontinuity;};
struct iptv_media {struct iptv_segment seg[512];size_t count;int end;double target;};
static void iptv_media_free(struct iptv_media *m){for(size_t i=0;i<m->count;i++){free(m->seg[i].uri);free(m->seg[i].map);}memset(m,0,sizeof *m);}
static int iptv_media_parse(char *text,struct iptv_media *m){
 memset(m,0,sizeof *m);m->target=3;unsigned long long seq=0;char *save=NULL,*active_map=NULL;int ext=0,discontinuity=0;
 for(char *p=strtok_r(text,"\n",&save);p;p=strtok_r(NULL,"\n",&save)){p=iptv_trim(p);
 if(!strncmp(p,"#EXT-X-MEDIA-SEQUENCE:",22)){char *end;seq=strtoull(p+22,&end,10);if(*end){free(active_map);return fail("Malformed HLS media sequence");}}
 else if(!strncmp(p,"#EXT-X-TARGETDURATION:",22)){m->target=atof(p+22);if(m->target<1)m->target=1;if(m->target>10)m->target=10;}
 else if(!strncmp(p,"#EXT-X-KEY:",11)){char *method=iptv_attr(p+11,"METHOD");int bad=!method||strcmp(method,"NONE");free(method);if(bad){free(active_map);return fail("Encrypted HLS stream unsupported");}}
 else if(!strncmp(p,"#EXT-X-MAP:",11)){char *uri=iptv_attr(p+11,"URI"),*range=iptv_attr(p+11,"BYTERANGE");if(!uri||!*uri||strlen(uri)>IPTV_URL_CAP||!range||*range){free(uri);free(range);free(active_map);return fail("Invalid or byte-range HLS initialization map");}free(range);free(active_map);active_map=uri;}
 else if(!strncmp(p,"#EXT-X-BYTERANGE:",17)){free(active_map);return fail("HLS byte ranges unsupported");}
 else if(!strcmp(p,"#EXT-X-DISCONTINUITY"))discontinuity=1;
 else if(!strcmp(p,"#EXT-X-ENDLIST"))m->end=1;
 else if(!strncmp(p,"#EXTINF:",8))ext=1;
 else if(*p&&*p!='#'){if(!ext){free(active_map);return fail("Malformed HLS media playlist");}if(m->count==512||strlen(p)>IPTV_URL_CAP){free(active_map);return fail("HLS playlist exceeds bounds");}m->seg[m->count].seq=seq++;m->seg[m->count].uri=strdup(p);m->seg[m->count].map=active_map?strdup(active_map):NULL;m->seg[m->count].discontinuity=discontinuity;discontinuity=0;if(!m->seg[m->count].uri||(active_map&&!m->seg[m->count].map)){free(m->seg[m->count].uri);free(m->seg[m->count].map);free(active_map);return fail("out of memory");}m->count++;ext=0;}}
 free(active_map);if(!m->count)return fail("No HLS segments available");return 0;
}
static int iptv_variant(char *text,const char *base,char *selected){
 unsigned long best=0;char *chosen=NULL,*save=NULL;int pending=0;unsigned long bandwidth=0;
 for(char *p=strtok_r(text,"\n",&save);p;p=strtok_r(NULL,"\n",&save)){p=iptv_trim(p);if(!strncmp(p,"#EXT-X-STREAM-INF:",18)){char *bw=iptv_attr(p+18,"BANDWIDTH"),*codecs=iptv_attr(p+18,"CODECS");bandwidth=bw?strtoul(bw,NULL,10):0;
 pending=codecs&&!strstr(codecs,"hvc1")&&!strstr(codecs,"hev1")&&!strstr(codecs,"vp09")&&!strstr(codecs,"av01")&&!strstr(codecs,"mp4a.40.5")&&!strstr(codecs,"mp4a.40.29")&&!strstr(codecs,"ec-3")&&(!*codecs||strstr(codecs,"avc1")||strstr(codecs,"avc3"))&&bandwidth<=16000000;free(bw);free(codecs);}
 else if(*p&&*p!='#'&&pending){int better=!chosen||(bandwidth<=8000000&&(best>8000000||bandwidth>best))||(bandwidth>8000000&&best>8000000&&bandwidth<best);if(better){free(chosen);chosen=strdup(p);best=bandwidth;}pending=0;}}
 if(!chosen)return fail("No compatible H.264 variant below 16 Mbps");int rc=iptv_resolve(selected,IPTV_URL_CAP+1,base,chosen);free(chosen);if(!rc)jf_log("IPTV HLS selected variant bandwidth %lu",best);return rc;
}
static int iptv_mp4_box(const unsigned char *p,size_t n,const char *box);
static int iptv_mp4_tag(const unsigned char *p,size_t n,const char *tag){
 for(size_t i=0;i+4<=n;i++)if(!memcmp(p+i,tag,4))return 1;return 0;
}
static int iptv_codec_candidate(const char *codecs){
 if(!codecs||!*codecs)return 1;
 if(strstr(codecs,"hvc1")||strstr(codecs,"hev1")||strstr(codecs,"vp09")||strstr(codecs,"av01")||strstr(codecs,"mp4a.40.5")||strstr(codecs,"mp4a.40.29")||strstr(codecs,"ec-3"))return 0;
 return strstr(codecs,"avc1")||strstr(codecs,"avc3");
}
static int iptv_probe_url(const struct iptv_channel *c,const char *url,char *selected,const char *token,int depth,int require_audio){
 if(depth>=5)return fail("Too many nested HLS playlists");
 struct sb body={0};char effective[IPTV_URL_CAP+1];if(iptv_download(url,c->ua,c->ref,token,&body,effective,1)){free(body.p);return -1;}
 if(!iptv_hls(body.p,body.len)){int ts=iptv_ts((unsigned char *)body.p,body.len);free(body.p);
  if(!ts)return fail("Unsupported direct stream container");snprintf(selected,IPTV_URL_CAP+1,"%s",url);return 0;}
 if(strstr(body.p,"#EXT-X-STREAM-INF:")){
  struct {char *url;unsigned long bandwidth;int require_audio;} variants[32]={0};size_t count=0;char *save=NULL;int pending=0,pending_audio=0;unsigned long bw=0;
  for(char *p=strtok_r(body.p,"\n",&save);p;p=strtok_r(NULL,"\n",&save)){p=iptv_trim(p);
   if(!strncmp(p,"#EXT-X-STREAM-INF:",18)){char *codecs=iptv_attr(p+18,"CODECS"),*bandwidth=iptv_attr(p+18,"BANDWIDTH"),*resolution=iptv_attr(p+18,"RESOLUTION"),*audio=iptv_attr(p+18,"AUDIO");
    bw=bandwidth?strtoul(bandwidth,NULL,10):0;unsigned width=0,height=0;if(resolution)sscanf(resolution,"%ux%u",&width,&height);
    pending=iptv_codec_candidate(codecs)&&bw<=16000000&&width<=1920&&height<=1080;
    pending_audio=audio&&*audio;
    if(!pending)jf_log("IPTV HLS candidate rejected: codec, resolution or bitrate incompatible");
    free(codecs);free(bandwidth);free(resolution);free(audio);
   }else if(*p&&*p!='#'&&pending){if(count<32){char resolved[IPTV_URL_CAP+1];if(!iptv_resolve(resolved,sizeof resolved,effective,p)){variants[count].url=strdup(resolved);variants[count].bandwidth=bw;variants[count].require_audio=pending_audio;if(variants[count].url)count++;}}pending=0;}
  }free(body.p);
  int rc=-1;char rejection[256]="No compatible rendition";for(size_t tries=0;tries<count&&tries<3;tries++){size_t best=count;unsigned long score=~0UL;
   for(size_t i=0;i<count;i++)if(variants[i].url){unsigned long rank=variants[i].bandwidth<=8000000?8000000-variants[i].bandwidth:16000000+variants[i].bandwidth;
    if(best==count||rank<score){best=i;score=rank;}}
   if(best==count)break;char *candidate=variants[best].url;variants[best].url=NULL;
   rc=iptv_probe_url(c,candidate,selected,token,depth+1,variants[best].require_audio);free(candidate);
   if(!rc)break;snprintf(rejection,sizeof rejection,"%s",g_err);jf_log("IPTV HLS candidate rejected: %s",rejection);
  }
  for(size_t i=0;i<count;i++)free(variants[i].url);
  return rc?fail("No playable HLS rendition: %s",rejection):0;
 }
 struct iptv_media m;int rc=iptv_media_parse(body.p,&m);free(body.p);if(rc){iptv_media_free(&m);return -1;}
 size_t index=m.end?0:m.count-1;char segment[IPTV_URL_CAP+1],actual[IPTV_URL_CAP+1];struct sb bytes={0};
 if(iptv_resolve(segment,sizeof segment,effective,m.seg[index].uri)||iptv_download(segment,c->ua,c->ref,token,&bytes,actual,1))rc=-1;
 else if(m.seg[index].map){struct sb init={0};char map_url[IPTV_URL_CAP+1],map_actual[IPTV_URL_CAP+1];
  if(!iptv_mp4_box((unsigned char *)bytes.p,bytes.len,"moof"))rc=fail("Unsupported HLS segment format (expected fragmented MP4)");
  else if(iptv_resolve(map_url,sizeof map_url,effective,m.seg[index].map)||iptv_download(map_url,c->ua,c->ref,token,&init,map_actual,1))rc=-1;
  else if(!iptv_mp4_box((unsigned char *)init.p,init.len,"moov"))rc=fail("Invalid HLS fragmented MP4 initialization");
  else if(iptv_mp4_tag((unsigned char *)init.p,init.len,"hvc1")||iptv_mp4_tag((unsigned char *)init.p,init.len,"hev1"))rc=fail("Unsupported video codec: HEVC");
  else if(!iptv_mp4_tag((unsigned char *)init.p,init.len,"avc1")&&!iptv_mp4_tag((unsigned char *)init.p,init.len,"avc3"))rc=fail("Unsupported HLS fragmented MP4 video codec");
  else if(require_audio&&!iptv_mp4_tag((unsigned char *)init.p,init.len,"soun"))rc=fail("Separate HLS audio rendition unsupported");
  free(init.p);
 }else if(!iptv_ts((unsigned char *)bytes.p,bytes.len))rc=fail("Unsupported HLS segment format (expected MPEG-TS)");
 else if(iptv_h264((unsigned char *)bytes.p,bytes.len))rc=-1;
 else if(require_audio){unsigned video,audio;iptv_program((unsigned char *)bytes.p,bytes.len,&video,&audio);if(audio==8192)rc=fail("Separate HLS audio rendition unsupported");}
 free(bytes.p);iptv_media_free(&m);if(rc)return -1;snprintf(selected,IPTV_URL_CAP+1,"%s",url);return 0;
}
static int iptv_probe(const struct iptv_channel *c,char *selected,const char *token){
 if(!strncmp(c->url,"srt:",4))return fail("Unsupported SRT stream");if(strstr(c->url,".mpd"))return fail("DASH stream unsupported");
 return iptv_probe_url(c,c->url,selected,token,0,0);
}
#ifndef REBOX_IPTV_MODULE
/* Transitional old-daemon wrapper. The module build prepares an inert plan. */
static int iptv_play(const char *id,struct sb *out){
 struct iptv_channel *c=iptv_find(id);if(!c)return fail("Unknown IPTV channel ID");
 state_lock();int busy=S->playing||S->iptv_active||S->yt_active;state_unlock();if(busy)return fail("Stop current playback before changing channels");
 char token[33];if(random_hex(token,32))return fail("Unable to create IPTV session");
 state_lock();if(S->playing||S->iptv_active||S->yt_active){state_unlock();return fail("Playback already active");}S->iptv_active=1;snprintf(S->iptv_token,sizeof S->iptv_token,"%s",token);state_unlock();
 char selected[IPTV_URL_CAP+1];if(iptv_probe(c,selected,token)){state_lock();if(!strcmp(S->iptv_token,token))S->iptv_active=0;state_unlock();return -1;}
 state_lock();if(S->playing||!S->iptv_active||strcmp(S->iptv_token,token)){state_unlock();return fail("IPTV start cancelled");}S->iptv_active=1;S->iptv_claimed=0;S->iptv_worker=0;snprintf(S->iptv_token,sizeof S->iptv_token,"%s",token);snprintf(S->iptv_url,sizeof S->iptv_url,"%s",selected);snprintf(S->iptv_ua,sizeof S->iptv_ua,"%s",c->ua);snprintf(S->iptv_ref,sizeof S->iptv_ref,"%s",c->ref);S->iptv_error[0]=0;unsigned long generation=playback_begin_locked("",c->name,c->id,0,1);S->play_live=2;S->ui_return_source=0;S->ui_key_until=mono_now()+6;state_unlock();
 char local[160];snprintf(local,sizeof local,"http://127.0.0.1:%d/iptv-stream/%s.ts",S->listen_port,token);
 if(invoke_play_url(local)){state_lock();playback_end_locked();state_unlock();return -1;}playback_setup_complete();
 for(double waited=0;waited<15;waited+=0.1){state_lock();int pending=S->iptv_active&&!S->iptv_claimed;state_unlock();if(!pending)break;nap(0.1);}
 state_lock();int active=S->iptv_active,claimed=S->iptv_claimed;if(active&&!claimed){snprintf(S->iptv_error,sizeof S->iptv_error,"Receiver did not open IPTV stream");playback_end_locked();}state_unlock();
 if(active&&!claimed){receiver_stop();schedule_tv_return(1,POST_STOP_DISMISS);return fail("Receiver did not open IPTV stream");}if(!active)return fail("IPTV stream failed: %s",S->iptv_error);
 jf_log("IPTV playback started: %s (%s, generation %lu)",c->name,c->id,generation);sb_puts(out,"{\"playing\":true,\"source\":\"iptv\",\"live\":true,\"channelId\":");sb_json_str(out,c->id);sb_puts(out,",\"channelName\":");sb_json_str(out,c->name);sb_fmt(out,",\"generation\":%lu,",generation);sb_puts(out,"\"returnToTv\":true}");return 0;
}
#endif
static int iptv_send(int fd,const void *p,size_t n,const char *token){if(!iptv_active(token))return -1;return write_all_fd(fd,p,n);}
struct iptv_remux {pid_t pid;int input;char map[IPTV_URL_CAP+1];};
static void iptv_remux_end(struct iptv_remux *r,const char *token){
 if(r->input>=0)close(r->input);r->input=-1;
 if(r->pid>0){int status,done=0;for(int i=0;i<40&&iptv_active(token);i++){pid_t got=waitpid(r->pid,&status,WNOHANG);if(got==r->pid){done=1;break;}if(got<0&&errno!=EINTR){done=1;break;}nap(.05);}
  if(!done){kill(r->pid,SIGKILL);while(waitpid(r->pid,&status,0)<0&&errno==EINTR){}}r->pid=0;}r->map[0]=0;
}
static int iptv_remux_start(struct iptv_remux *r,int fd){
 int p[2];if(pipe(p))return fail("Unable to start IPTV remuxer");
 char helper[1024];
#ifdef REBOX_IPTV_MODULE
 snprintf(helper,sizeof helper,"%s/bin/remux",package_root);
#else
 snprintf(helper,sizeof helper,"%s/iptv/bin/remux",persist_root);
#endif
 const char *override=getenv("IPTV_REMUX_CMD");if(override&&*override)snprintf(helper,sizeof helper,"%s",override);
 pid_t owner=getpid();r->pid=fork();if(r->pid<0){close(p[0]);close(p[1]);return fail("Unable to start IPTV remuxer");}
 if(!r->pid){prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()!=owner)_exit(127);dup2(p[0],0);dup2(fd,1);close(p[0]);close(p[1]);for(int inherited=3;inherited<1024;inherited++)close(inherited);execl(helper,helper,(char *)NULL);_exit(127);}
 close(p[0]);r->input=p[1];int flags=fcntl(r->input,F_GETFL,0);if(flags>=0)fcntl(r->input,F_SETFL,flags|O_NONBLOCK);return 0;
}
static int iptv_remux_feed(struct iptv_remux *r,const void *data,size_t size,const char *token){
 const unsigned char *p=data;double until=mono_now()+20;
 while(size){if(!iptv_active(token))return fail("IPTV stopped");ssize_t n=write(r->input,p,size>32768?32768:size);
  if(n>0){p+=n;size-=n;until=mono_now()+20;continue;}
  if(n<0&&errno==EINTR)continue;if(n<0&&errno!=EAGAIN)return fail("IPTV remuxer stopped");
  struct pollfd wait={r->input,POLLOUT,0};poll(&wait,1,100);if(wait.revents&(POLLERR|POLLHUP)||mono_now()>until)return fail("IPTV remuxer stalled");
 }return 0;
}
static int iptv_mp4_box(const unsigned char *p,size_t n,const char *box){
 for(size_t at=0;at+8<=n&&at<256;){unsigned len=(p[at]<<24)|(p[at+1]<<16)|(p[at+2]<<8)|p[at+3];
  if(!memcmp(p+at+4,box,4))return 1;if(len<8||len>n-at)break;at+=len;
 }return 0;
}
static int iptv_retryable(void){return strstr(g_err,"HTTP 403")||strstr(g_err,"HTTP 404")||strstr(g_err,"HTTP 429")||strstr(g_err,"Provider error")||strstr(g_err,"connection")||strstr(g_err,"network error")||strstr(g_err,"TLS")||strstr(g_err,"timed out")||strstr(g_err,"Stream ended");}
static int iptv_retry(const char *token,int *failures){
 if(!iptv_active(token)||!iptv_retryable()||*failures>=3)return 0;
 int delay=1<<(*failures)++;jf_log("IPTV upstream interruption: %s; reconnect attempt %d/3",g_err,*failures);
 state_lock();if(S->iptv_active&&!strcmp(S->iptv_token,token))snprintf(S->iptv_error,sizeof S->iptv_error,"Playback interrupted; reconnecting (%d/3)",*failures);state_unlock();
 for(int t=0;t<delay*10&&iptv_active(token);t++)nap(.1);return iptv_active(token);
}
static void iptv_stream(int fd,const char *path,const char *method){
 if(strcmp(method,"GET")||strlen(path)!=35||strcmp(path+32,".ts")){send_json_error(fd,404,"Unknown IPTV stream",method);return;}char token[33];memcpy(token,path,32);token[32]=0;
 char url[IPTV_URL_CAP+1],ua[2049],ref[2049];state_lock();if(!S->iptv_active||strcmp(token,S->iptv_token)||S->iptv_claimed){state_unlock();send_json_error(fd,404,"Expired IPTV stream",method);return;}S->iptv_claimed=1;S->iptv_worker=getpid();snprintf(url,sizeof url,"%s",S->iptv_url);snprintf(ua,sizeof ua,"%s",S->iptv_ua);snprintf(ref,sizeof ref,"%s",S->iptv_ref);state_unlock();alarm(0);
 struct timeval timeout={3,0};setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
 int started=0,rc=0,failures=0,good_segments=0;unsigned long long next=0;int have_next=0;double progressed=mono_now();struct iptv_remux remux={.input=-1};
 while(iptv_active(token)){
 struct iptv_fetch f;if(iptv_open(&f,url,ua,ref,0,token)){rc=-1;if(started&&iptv_retry(token,&failures)){rc=0;continue;}break;}struct sb b={0};char chunk[32768];int hls=0;
 for(;;){ssize_t n=iptv_read(&f,chunk,sizeof chunk,token);if(n<=0){rc=n<0?-1:0;break;}if(sb_putn(&b,chunk,(size_t)n)){rc=-1;break;}if(b.len>=376){hls=iptv_hls(b.p,b.len);if(!hls)break;}if(b.len>1024*1024){rc=fail("HLS playlist exceeds bounds");break;}}
 hls=iptv_hls(b.p,b.len);
 if(!rc&&!hls){int downstream=0;double direct_since=mono_now();size_t direct_bytes=b.len;
  if(!iptv_ts((unsigned char *)b.p,b.len))rc=fail("Unsupported stream format");
  else{double prebuffer_since=mono_now();
   /* A direct TS connection often begins mid-GOP. FilmBox's first complete
    * keyframe was 660 KiB into a sampled stream; feeding only the first PMT
    * made playURL start decoding incomplete H.264 access units. Keep a bounded
    * burst ready before handing the connection to the receiver. */
   while(b.len<1024*1024&&mono_now()-prebuffer_since<5&&iptv_active(token)){
    ssize_t n=iptv_read(&f,chunk,sizeof chunk,token);if(n<0){rc=-1;break;}if(!n)break;
    if(sb_putn(&b,chunk,(size_t)n)){rc=-1;break;}
   }
   if(!rc&&iptv_h264((unsigned char *)b.p,b.len))rc=-1;else if(!rc){g_err[0]=0;if(!started){const char *head="HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";rc=iptv_send(fd,head,strlen(head),token);started=1;}
    if(!rc){rc=iptv_send(fd,b.p,b.len,token);downstream=rc!=0;}free(b.p);b.p=NULL;
    while(!rc&&iptv_active(token)){ssize_t n=iptv_read(&f,chunk,sizeof chunk,token);if(n<=0){rc=n<0?-1:0;break;}direct_bytes+=(size_t)n;rc=iptv_send(fd,chunk,(size_t)n,token);if(rc)downstream=1;if(mono_now()-progressed>60){failures=0;progressed=mono_now();}}
   }}
  iptv_fetch_close(&f);free(b.p);
  if(started&&!downstream&&failures<3&&iptv_active(token)&&(direct_bytes>64*1024||mono_now()-direct_since>2)){fail("Stream ended unexpectedly");if(iptv_retry(token,&failures)){rc=0;continue;}}
  break;
 }
 char effective[IPTV_URL_CAP+1];snprintf(effective,sizeof effective,"%s",f.url);iptv_fetch_close(&f);if(rc){free(b.p);if(started&&iptv_retry(token,&failures)){rc=0;continue;}break;}
 if(strstr(b.p,"#EXT-X-STREAM-INF:")){rc=iptv_variant(b.p,effective,url);free(b.p);if(rc)break;continue;}
 struct iptv_media m;rc=iptv_media_parse(b.p,&m);free(b.p);if(rc){iptv_media_free(&m);break;}
 if(!have_next&&!m.end&&m.count>3){next=m.seg[m.count-3].seq;have_next=1;}
 for(size_t i=0;i<m.count&&iptv_active(token);i++){if(have_next&&m.seg[i].seq<next)continue;char segment[IPTV_URL_CAP+1],actual[IPTV_URL_CAP+1];struct sb bytes={0};if(iptv_resolve(segment,sizeof segment,effective,m.seg[i].uri)||iptv_download(segment,ua,ref,token,&bytes,actual,0)){free(bytes.p);rc=-1;break;}
 int mp4=m.seg[i].map!=NULL;
 if(mp4){if(!iptv_mp4_box((unsigned char *)bytes.p,bytes.len,"moof")){free(bytes.p);rc=fail("Unsupported HLS segment format (expected fragmented MP4)");break;}}
 else if(!iptv_ts((unsigned char *)bytes.p,bytes.len)){free(bytes.p);rc=fail("Unsupported HLS segment format (expected MPEG-TS)");break;}
 else if(iptv_h264((unsigned char *)bytes.p,bytes.len)){free(bytes.p);rc=-1;break;}
 if(!started&&!mp4)iptv_audio_startup((unsigned char *)bytes.p,bytes.len);
 if(!started){const char *head="HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";rc=iptv_send(fd,head,strlen(head),token);started=1;}
 if(mp4&&!rc&&(!remux.pid||m.seg[i].discontinuity||strcmp(remux.map,m.seg[i].map))){
  if(remux.pid)iptv_remux_end(&remux,token);
  struct sb init={0};char map_url[IPTV_URL_CAP+1],map_actual[IPTV_URL_CAP+1];
  if(iptv_resolve(map_url,sizeof map_url,effective,m.seg[i].map)||iptv_download(map_url,ua,ref,token,&init,map_actual,0))rc=-1;
  else if(!iptv_mp4_box((unsigned char *)init.p,init.len,"moov"))rc=fail("Invalid HLS fragmented MP4 initialization");
  else if(iptv_remux_start(&remux,fd))rc=-1;
  else{snprintf(remux.map,sizeof remux.map,"%s",m.seg[i].map);rc=iptv_remux_feed(&remux,init.p,init.len,token);}
  free(init.p);
 }
 if(!rc)rc=mp4?iptv_remux_feed(&remux,bytes.p,bytes.len,token):iptv_send(fd,bytes.p,bytes.len,token);
 jf_log("IPTV segment seq=%llu bytes=%zu container=%s",m.seg[i].seq,bytes.len,mp4?"fMP4":"TS");free(bytes.p);if(rc)break;next=m.seg[i].seq+1;have_next=1;progressed=mono_now();if(++good_segments>=3){failures=good_segments=0;}}
 int end=m.end;double target=m.target;iptv_media_free(&m);if(rc){if(started&&iptv_retry(token,&failures)){rc=0;continue;}break;}if(end)break;
 if(mono_now()-progressed>40){rc=fail("Live stream stalled");if(iptv_retry(token,&failures)){rc=0;continue;}break;}
 for(double t=0;t<target/2&&iptv_active(token);t+=0.1)nap(0.1);
 }
 iptv_remux_end(&remux,token);
 if(!started&&iptv_active(token))send_json_error(fd,502,g_err[0]?g_err:"Stream ended",method);
 state_lock();int owned=S->iptv_active&&!strcmp(S->iptv_token,token);if(owned){snprintf(S->iptv_error,sizeof S->iptv_error,"%s",rc&&g_err[0]?g_err:"Stream ended");int back=S->return_to_tv;playback_end_locked();state_unlock();jf_log("IPTV relay ended: %s",S->iptv_error);
#ifndef REBOX_IPTV_MODULE
receiver_stop();schedule_tv_return(back,POST_STOP_DISMISS);
#else
(void)back;
#endif
}else state_unlock();
}
static int iptv_status(struct sb *out){sb_fmt(out,"{\"loaded\":%s,\"channels\":%zu,\"active\":%s,\"workerPid\":%d,\"error\":",iptv_loaded?"true":"false",iptv_count,S->iptv_active?"true":"false",S->iptv_worker);sb_json_str(out,iptv_library_error[0]?iptv_library_error:S->iptv_error);sb_puts(out,"}");return 0;}

/* Separate native checkpoint; never reads or overwrites Jellyfin state. */
static int iptv_state(struct jval *payload,struct sb *out){
 char path[512];snprintf(path,sizeof path,"%s/state/iptv-state.json",persist_root);
 if(!payload){size_t n=0;char *raw=read_file(path,&n);struct jval *v=raw&&n<=4096?json_parse(raw,n):NULL;int ok=v&&v->t==J_OBJ;if(ok)sb_putn(out,raw,n);else sb_puts(out,"{}");free(raw);jfree(v);return 0;}
 struct sb b={0};sb_puts(&b,"{");const char *strings[]={"mode","view","group","query","submitted","channelId"};
 for(size_t i=0;i<6;i++){const char *v=jstr(jget(payload,strings[i]));char value[1025];snprintf(value,sizeof value,"%.*s",i==2?1024:(i==3||i==4?256:32),v?v:"");if(i)sb_puts(&b,",");sb_json_str(&b,strings[i]);sb_puts(&b,":");sb_json_str(&b,value);}
 const char *numbers[]={"page","index","rootPage","rootIndex"};for(size_t i=0;i<4;i++){double v=jnum(jget(payload,numbers[i]),0);if(!(v>=0))v=0;if(v>(i%2?5:1667))v=i%2?5:1667;sb_puts(&b,",");sb_json_str(&b,numbers[i]);sb_fmt(&b,":%d",(int)v);}sb_puts(&b,"}");int rc=atomic_write(path,b.p,b.len,0600);free(b.p);if(rc)return fail("Unable to save IPTV browse state");sb_puts(out,"{\"saved\":true}");return 0;
}
