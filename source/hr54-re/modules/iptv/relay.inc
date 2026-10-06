/* One receiver-native IPTV session. Networking uses installed TLS adapter;
 * no video is written to disk and no codec processing/transcoding occurs. */
struct iptv_fetch {int fd;pid_t pid;char url[IPTV_URL_CAP+1],ct[257];};
static int iptv_active(const char *token){int ok;state_lock();ok=S->iptv_active&&!strcmp(S->iptv_token,token);state_unlock();return ok;}
static void iptv_fetch_close(struct iptv_fetch *f){if(f->fd>=0)close(f->fd);if(f->pid>0){kill(f->pid,SIGKILL);while(waitpid(f->pid,NULL,0)<0&&errno==EINTR){}}f->fd=-1;f->pid=0;}
static ssize_t iptv_read(struct iptv_fetch *f,void *buf,size_t cap,const char *token){
 double until=mono_now()+20;for(;;){if(token&&!iptv_active(token))return fail("IPTV stopped");struct pollfd p={f->fd,POLLIN,0};int r=poll(&p,1,100);if(r<0&&errno==EINTR)continue;if(r<0)return -1;if(r){ssize_t n=read(f->fd,buf,cap);if(n<0&&errno==EINTR)continue;return n;}if(mono_now()>until)return fail("Channel unreachable or TLS connection failed");}}
static int iptv_open(struct iptv_fetch *f,const char *url,const char *ua,const char *ref,int timeout,const char *token){
 memset(f,0,sizeof *f);f->fd=-1;if(strlen(url)>IPTV_URL_CAP||(strncmp(url,"http://",7)&&strncmp(url,"https://",8))||strpbrk(url,"\r\n"))return fail("Unsupported IPTV protocol");
 char helper[512],ca[512],seconds[16];snprintf(helper,sizeof helper,"%s/bin/hr54-iptv-fetch",persist_root);
#ifdef REBOX_IPTV_MODULE
 snprintf(helper,sizeof helper,"%s/bin/fetch",package_root);
#endif
 const char *env=getenv("IPTV_FETCH_CMD");if(env&&*env)snprintf(helper,sizeof helper,"%s",env);snprintf(ca,sizeof ca,"%s/iptv/ca-certificates.crt",persist_root);snprintf(seconds,sizeof seconds,"%d",timeout);
 pid_t owner=getpid();int p[2];if(pipe(p))return fail("Unable to start TLS helper");f->pid=fork();if(f->pid<0){close(p[0]);close(p[1]);return fail("Unable to start TLS helper");}
 if(!f->pid){prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()!=owner)_exit(127);alarm(0);dup2(p[1],1);close(p[0]);close(p[1]);for(int inherited=3;inherited<1024;inherited++)close(inherited);execl(helper,helper,url,ca,ua,ref,seconds,(char *)NULL);_exit(127);}close(p[1]);f->fd=p[0];
 char header[IPTV_URL_CAP+260];size_t n=0;int newlines=0;while(n<sizeof header-1){char c;ssize_t r=iptv_read(f,&c,1,token);if(r<=0){iptv_fetch_close(f);return fail("Channel unavailable or TLS connection failed (see fetch log)");}header[n++]=c;if(c=='\n'&&++newlines==3)break;}header[n]=0;
 char *ct=strchr(header,'\n');if(!ct||n>=sizeof header-1){iptv_fetch_close(f);return fail("Invalid upstream response");}*ct++=0;char *end=strchr(ct,'\n');if(end)*end=0;if(strlen(header)>IPTV_URL_CAP||strlen(ct)>256){iptv_fetch_close(f);return fail("Upstream headers exceed bounds");}snprintf(f->url,sizeof f->url,"%s",header);snprintf(f->ct,sizeof f->ct,"%s",ct);return 0;
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
/* Inspect complete PMT sections in TS packets; split PMTs fail safely. */
static int iptv_h264(const unsigned char *p,size_t n){
 for(size_t at=0;at+188<=n;at+=188){const unsigned char *t=p+at;if(t[0]!=0x47||!(t[1]&0x40))continue;int af=(t[3]>>4)&3;if(!(af&1))continue;size_t off=4;if(af&2)off+=1+t[4];if(off>=187)continue;off+=1+t[off];if(off+12>188||t[off]!=2)continue;size_t end=off+3+(((t[off+1]&15)<<8)|t[off+2]);if(end>188||end<off+16)continue;size_t es=off+12+(((t[off+10]&15)<<8)|t[off+11]);while(es+5<=end-4){unsigned type=t[es];if(type==0x1b){jf_log("IPTV TS recognized MPEG-TS, H.264 video PID %u (PMT stream_type 0x1b)",((t[es+1]&31)<<8)|t[es+2]);return 0;}es+=5+(((t[es+3]&15)<<8)|t[es+4]);}}
 return fail("Unsupported video codec or no complete H.264 program map");
}
/* Stock playURL probes only the start of TS. Some HLS encoders place the
 * first AAC PES behind a >1 MiB video access unit. Move that complete PES
 * before the first video PES, preserving every packet and per-PID order.
 * This runs once on the already bounded first segment; no codec processing. */
static int iptv_audio_startup(unsigned char *p,size_t n){
 if(n%188)return 0;
 unsigned video=8192,audio=8192;
 for(size_t at=0;at+188<=n;at+=188){unsigned char *t=p+at;if(t[0]!=0x47||!(t[1]&64)||!(t[3]&16))continue;size_t off=4;if(t[3]&32)off+=1+t[4];if(off>=187)continue;off+=1+t[off];if(off+12>188||t[off]!=2)continue;size_t end=off+3+(((t[off+1]&15)<<8)|t[off+2]);if(end>188||end<off+16)continue;size_t es=off+12+(((t[off+10]&15)<<8)|t[off+11]);while(es+5<=end-4){unsigned pid=((t[es+1]&31)<<8)|t[es+2];if(t[es]==0x1b)video=pid;if(audio==8192&&(t[es]==0x0f||t[es]==0x81))audio=pid;es+=5+(((t[es+3]&15)<<8)|t[es+4]);}if(video!=8192&&audio!=8192)break;}
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
 struct iptv_fetch f;if(iptv_open(&f,url,ua,ref,20,token))return -1;snprintf(effective,IPTV_URL_CAP+1,"%s",f.url);size_t cap=probe?1024*1024:12*1024*1024;int rc=0;char buf[32768];for(;;){ssize_t n=iptv_read(&f,buf,sizeof buf,token);if(n<0){rc=-1;break;}if(!n)break;if(body->len+(size_t)n>cap){rc=fail("Upstream playlist/segment exceeds bounds");break;}if(sb_putn(body,buf,(size_t)n)){rc=-1;break;}if(probe&&body->len>=376&&!iptv_hls(body->p,body->len))break;}
 if(!rc&&!body->len)rc=fail("Channel returned no data");iptv_fetch_close(&f);return rc;
}
struct iptv_segment {unsigned long long seq;char *uri;};
struct iptv_media {struct iptv_segment seg[512];size_t count;int end;double target;};
static void iptv_media_free(struct iptv_media *m){for(size_t i=0;i<m->count;i++)free(m->seg[i].uri);memset(m,0,sizeof *m);}
static int iptv_media_parse(char *text,struct iptv_media *m){
 memset(m,0,sizeof *m);m->target=3;unsigned long long seq=0;char *save=NULL;int ext=0;
 for(char *p=strtok_r(text,"\n",&save);p;p=strtok_r(NULL,"\n",&save)){p=iptv_trim(p);
 if(!strncmp(p,"#EXT-X-MEDIA-SEQUENCE:",22)){char *end;seq=strtoull(p+22,&end,10);if(*end)return fail("Malformed HLS media sequence");}
 else if(!strncmp(p,"#EXT-X-TARGETDURATION:",22)){m->target=atof(p+22);if(m->target<1)m->target=1;if(m->target>10)m->target=10;}
 else if(!strncmp(p,"#EXT-X-KEY:",11)){char *method=iptv_attr(p+11,"METHOD");int bad=!method||strcmp(method,"NONE");free(method);if(bad)return fail("Encrypted HLS stream unsupported");}
 else if(!strncmp(p,"#EXT-X-MAP:",11))return fail("Unsupported fMP4 HLS");
 else if(!strncmp(p,"#EXT-X-BYTERANGE:",17))return fail("HLS byte ranges unsupported");
 else if(!strcmp(p,"#EXT-X-ENDLIST"))m->end=1;
 else if(!strncmp(p,"#EXTINF:",8))ext=1;
 else if(*p&&*p!='#'){if(!ext)return fail("Malformed HLS media playlist");if(m->count==512||strlen(p)>IPTV_URL_CAP)return fail("HLS playlist exceeds bounds");m->seg[m->count].seq=seq++;m->seg[m->count].uri=strdup(p);if(!m->seg[m->count].uri)return fail("out of memory");m->count++;ext=0;}}
 if(!m->count)return fail("No HLS segments available");return 0;
}
static int iptv_variant(char *text,const char *base,char *selected){
 unsigned long best=0;char *chosen=NULL,*save=NULL;int pending=0;unsigned long bandwidth=0;
 for(char *p=strtok_r(text,"\n",&save);p;p=strtok_r(NULL,"\n",&save)){p=iptv_trim(p);if(!strncmp(p,"#EXT-X-STREAM-INF:",18)){char *bw=iptv_attr(p+18,"BANDWIDTH"),*codecs=iptv_attr(p+18,"CODECS");bandwidth=bw?strtoul(bw,NULL,10):0;
 pending=codecs&&!strstr(codecs,"hvc1")&&!strstr(codecs,"hev1")&&!strstr(codecs,"vp09")&&!strstr(codecs,"av01")&&!strstr(codecs,"mp4a.40.5")&&!strstr(codecs,"mp4a.40.29")&&!strstr(codecs,"ec-3")&&(!*codecs||strstr(codecs,"avc1")||strstr(codecs,"avc3"))&&bandwidth<=16000000;free(bw);free(codecs);}
 else if(*p&&*p!='#'&&pending){int better=!chosen||(bandwidth<=8000000&&(best>8000000||bandwidth>best))||(bandwidth>8000000&&best>8000000&&bandwidth<best);if(better){free(chosen);chosen=strdup(p);best=bandwidth;}pending=0;}}
 if(!chosen)return fail("No compatible H.264 variant below 16 Mbps");int rc=iptv_resolve(selected,IPTV_URL_CAP+1,base,chosen);free(chosen);if(!rc)jf_log("IPTV HLS selected variant bandwidth %lu",best);return rc;
}
static int iptv_probe(const struct iptv_channel *c,char *selected,const char *token){
 if(!strncmp(c->url,"srt:",4))return fail("Unsupported SRT stream");if(strstr(c->url,".mpd"))return fail("DASH stream not supported yet");
 snprintf(selected,IPTV_URL_CAP+1,"%s",c->url);for(int depth=0;depth<5;depth++){struct sb b={0};char effective[IPTV_URL_CAP+1];if(iptv_download(selected,c->ua,c->ref,token,&b,effective,1)){free(b.p);return -1;}
 if(!iptv_hls(b.p,b.len)){int ts=iptv_ts((unsigned char *)b.p,b.len);free(b.p);return ts?0:fail("Unsupported stream format (DASH/fMP4 or unavailable)");}
 if(strstr(b.p,"#EXT-X-STREAM-INF:")){int rc=iptv_variant(b.p,effective,selected);free(b.p);if(rc)return -1;continue;}
 struct iptv_media m;int rc=iptv_media_parse(b.p,&m);iptv_media_free(&m);free(b.p);if(rc)return -1;snprintf(selected,IPTV_URL_CAP+1,"%s",effective);return 0;}return fail("Too many nested HLS playlists");
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
static void iptv_stream(int fd,const char *path,const char *method){
 if(strcmp(method,"GET")||strlen(path)!=35||strcmp(path+32,".ts")){send_json_error(fd,404,"Unknown IPTV stream",method);return;}char token[33];memcpy(token,path,32);token[32]=0;
 char url[IPTV_URL_CAP+1],ua[2049],ref[2049];state_lock();if(!S->iptv_active||strcmp(token,S->iptv_token)||S->iptv_claimed){state_unlock();send_json_error(fd,404,"Expired IPTV stream",method);return;}S->iptv_claimed=1;S->iptv_worker=getpid();snprintf(url,sizeof url,"%s",S->iptv_url);snprintf(ua,sizeof ua,"%s",S->iptv_ua);snprintf(ref,sizeof ref,"%s",S->iptv_ref);state_unlock();alarm(0);
 struct timeval timeout={3,0};setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
 int started=0,rc=0;unsigned long long next=0;int have_next=0;double progressed=mono_now();
 while(iptv_active(token)){
 struct iptv_fetch f;if(iptv_open(&f,url,ua,ref,0,token)){rc=-1;break;}struct sb b={0};char chunk[32768];int hls=0;
 for(;;){ssize_t n=iptv_read(&f,chunk,sizeof chunk,token);if(n<=0){rc=n<0?-1:0;break;}if(sb_putn(&b,chunk,(size_t)n)){rc=-1;break;}if(b.len>=376){hls=iptv_hls(b.p,b.len);if(!hls)break;}if(b.len>1024*1024){rc=fail("HLS playlist exceeds bounds");break;}}
 hls=iptv_hls(b.p,b.len);
 if(!rc&&!hls){if(!iptv_ts((unsigned char *)b.p,b.len))rc=fail("Unsupported stream format");else if(iptv_h264((unsigned char *)b.p,b.len))rc=-1;else{if(!started){const char *head="HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";rc=iptv_send(fd,head,strlen(head),token);started=1;}if(!rc)rc=iptv_send(fd,b.p,b.len,token);free(b.p);b.p=NULL;while(!rc&&iptv_active(token)){ssize_t n=iptv_read(&f,chunk,sizeof chunk,token);if(n<=0){rc=n<0?-1:0;break;}rc=iptv_send(fd,chunk,(size_t)n,token);}}iptv_fetch_close(&f);free(b.p);break;}
 char effective[IPTV_URL_CAP+1];snprintf(effective,sizeof effective,"%s",f.url);iptv_fetch_close(&f);if(rc){free(b.p);break;}
 if(strstr(b.p,"#EXT-X-STREAM-INF:")){rc=iptv_variant(b.p,effective,url);free(b.p);if(rc)break;continue;}
 struct iptv_media m;rc=iptv_media_parse(b.p,&m);free(b.p);if(rc){iptv_media_free(&m);break;}
 if(!have_next&&!m.end&&m.count>3){next=m.seg[m.count-3].seq;have_next=1;}
 for(size_t i=0;i<m.count&&iptv_active(token);i++){if(have_next&&m.seg[i].seq<next)continue;char segment[IPTV_URL_CAP+1],actual[IPTV_URL_CAP+1];struct sb bytes={0};if(iptv_resolve(segment,sizeof segment,effective,m.seg[i].uri)||iptv_download(segment,ua,ref,token,&bytes,actual,0)){free(bytes.p);rc=-1;break;}
 if(!iptv_ts((unsigned char *)bytes.p,bytes.len)){free(bytes.p);rc=fail("Unsupported fMP4 HLS or invalid TS segment");break;}
 if(iptv_h264((unsigned char *)bytes.p,bytes.len)){free(bytes.p);rc=-1;break;}
 if(!started)iptv_audio_startup((unsigned char *)bytes.p,bytes.len);
 if(!started){const char *head="HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";rc=iptv_send(fd,head,strlen(head),token);started=1;}
 if(!rc)rc=iptv_send(fd,bytes.p,bytes.len,token);jf_log("IPTV segment seq=%llu bytes=%zu",m.seg[i].seq,bytes.len);free(bytes.p);if(rc)break;next=m.seg[i].seq+1;have_next=1;progressed=mono_now();}
 int end=m.end;double target=m.target;iptv_media_free(&m);if(rc||end)break;if(mono_now()-progressed>40){rc=fail("Live stream stalled");break;}for(double t=0;t<target/2&&iptv_active(token);t+=0.1)nap(0.1);
 }
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
