/* On-demand receiver-native resolver and packet-copy worker. */
/* The receiver needs 4-7s from requesting the stream to presenting its first
 * frame, and the demuxer only reports started after that. play_started is
 * stamped before playURL is even issued, so the post-stream hold must run
 * from the claim instead, or long videos lose their last seconds. */
#define YT_STARTUP_GRACE 15.0
#define YT_DRAIN_GRACE 2.0
static int yt_valid_id(const char *s){if(!s||strlen(s)!=11)return 0;for(int i=0;i<11;i++)if(!isalnum((unsigned char)s[i])&&s[i]!='_'&&s[i]!='-')return 0;return 1;}
static int yt_active(const char *token){state_lock();int ok=S->yt_active&&!strcmp(S->yt_token,token);state_unlock();return ok;}
static int yt_reap(pid_t pid){if(pid<=1)return -1;int status=0;pid_t got=0;
 /* Give EOF cleanup a bounded chance to exit so stderr/exit diagnostics keep
  * the worker's real status. Cancellation remains bounded and concurrent. */
 for(int i=0;i<20;i++){got=waitpid(pid,&status,WNOHANG);if(got!=0)break;nap(.01);}
 if(got==0){kill(-pid,SIGKILL);kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}}
char dir[160],path[180];snprintf(dir,sizeof dir,"/tmp/hr54-youtube-%ld",(long)pid);snprintf(path,sizeof path,"%s/video",dir);unlink(path);snprintf(path,sizeof path,"%s/audio",dir);unlink(path);rmdir(dir);return got<0?-1:status;}
static pid_t yt_spawn(int *readfd,const char *mode,const char *arg,const char *page,int relay){
 char root[512],exe[512],script[512],home[512],ca[512];snprintf(root,sizeof root,"%s/youtube",persist_root);snprintf(exe,sizeof exe,"%s/bin/%s",root,relay?"relay":"python");snprintf(script,sizeof script,"%s/resolver.pyc",root);snprintf(home,sizeof home,"%s/python",root);snprintf(ca,sizeof ca,"%s/iptv/ca-certificates.crt",persist_root);

#if defined(REBOX_YOUTUBE_MODULE) && !defined(REBOX_HOST_TEST)
 const char *package=getenv("REBOX_MODULE_PACKAGE");if(package){snprintf(script,sizeof script,"%s/bin/resolver.pyc",package);if(relay)snprintf(exe,sizeof exe,"%s/bin/relay",package);snprintf(ca,sizeof ca,"%s/youtube/ca-certificates.crt",persist_root);}
#endif
 if(access(exe,X_OK)||(!relay&&access(script,R_OK))){fprintf(stderr,"YouTube worker missing path=%s script=%s\n",exe,relay?"[none]":script);fail("Resolver unavailable");return -1;}fprintf(stderr,"YouTube worker path=%s mode=%s script=%s\n",exe,relay?"packet-copy":mode,relay?"[none]":script);int p[2];if(pipe(p)){fail("Unable to create YouTube pipe");return -1;}pid_t owner=getpid(),pid=fork();if(pid<0){close(p[0]);close(p[1]);return -1;}
 if(!pid){setpgid(0,0);prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()!=owner)_exit(127);alarm(0);dup2(p[1],1);close(p[0]);close(p[1]);for(int i=3;i<1024;i++)close(i);setenv("PYTHONHOME",home,1);setenv("REBOX_YOUTUBE_RUNTIME",root,1);
#ifdef REBOX_YOUTUBE_MODULE
const char *package=getenv("REBOX_MODULE_PACKAGE");if(package){char fetch[1024];snprintf(fetch,sizeof fetch,"%s/bin/fetch",package);setenv("REBOX_YOUTUBE_FETCH",fetch,1);}
#endif
setenv("SSL_CERT_FILE",ca,1);setenv("PYTHONDONTWRITEBYTECODE","1",1);if(relay)execl(exe,exe,arg,page,(char *)NULL);else execl(exe,exe,script,mode,arg,page,(char *)NULL);_exit(127);}setpgid(pid,pid);close(p[1]);*readfd=p[0];return pid;
}
static int yt_resolve(const char *mode,const char *arg,const char *page,const char *token,struct sb *out){
 int fd;pid_t pid=yt_spawn(&fd,mode,arg,page,0);if(pid<0)return -1;double until=mono_now()+60;int rc=0;char buf[8192];for(;;){if(!yt_active(token)){rc=fail("YouTube cancelled");break;}if(mono_now()>until){rc=fail("YouTube resolver timed out");break;}struct pollfd p={fd,POLLIN,0};int n=poll(&p,1,100);if(n<0&&errno==EINTR)continue;if(n<0){rc=-1;break;}if(!n)continue;ssize_t k=read(fd,buf,sizeof buf);if(k<0&&errno==EINTR)continue;if(k<=0){if(k<0)rc=-1;break;}if(out->len+(size_t)k>32768){rc=fail("YouTube response exceeds bounds");break;}if(sb_putn(out,buf,k)){rc=-1;break;}}
 close(fd);int status=yt_reap(pid);fprintf(stderr,"YouTube resolver exit=%d signal=%d bytes=%zu\n",status>=0&&WIFEXITED(status)?WEXITSTATUS(status):-1,status>=0&&WIFSIGNALED(status)?WTERMSIG(status):0,out->len);if(rc)return rc;struct jval *v=json_parse(out->p,out->len);const char *error=jstr(jget(v,"error"));if(!v)rc=fail("Resolver failed");else if(error&&*error)rc=fail("%s",error);jfree(v);return rc;
}
#ifndef REBOX_YOUTUBE_MODULE
static int yt_claim(char token[33]){if(random_hex(token,32))return -1;state_lock();if(S->playing||S->iptv_active||S->yt_active){state_unlock();return fail("Stop current playback before using YouTube");}S->yt_active=1;S->yt_claimed=0;S->yt_claim_at=0;S->yt_error[0]=0;snprintf(S->yt_token,sizeof S->yt_token,"%s",token);state_unlock();return 0;}
static int yt_search(const char *query,struct sb *out){char q[65],page[16],token[33];query_param(query,"q",q,sizeof q);query_param(query,"page",page,sizeof page);int p=atoi(page);if(p<0||p>100||!*q)return fail("Invalid YouTube search");snprintf(page,sizeof page,"%d",p);if(yt_claim(token))return -1;int rc=yt_resolve("search",q,page,token,out);state_lock();if(!strcmp(S->yt_token,token))S->yt_active=0;state_unlock();return rc;}
#endif
static int yt_status(struct sb *out){char path[512];snprintf(path,sizeof path,"%s/youtube/resolver.pyc",persist_root);state_lock();sb_fmt(out,"{\"available\":%s,\"active\":%s,\"playing\":%s,\"error\":",access(path,R_OK)?"false":"true",S->yt_active?"true":"false",S->playing&&S->play_live==3?"true":"false");sb_json_str(out,S->yt_error);sb_puts(out,"}");state_unlock();return 0;}
#ifndef REBOX_YOUTUBE_MODULE
static int yt_play(const char *id,struct sb *out){unsigned long generation=0;if(!yt_valid_id(id))return fail("Invalid video ID");char token[33];if(yt_claim(token))return -1;struct sb result={0};int rc=yt_resolve("play",id,"0",token,&result);struct jval *v=rc?NULL:json_parse(result.p,result.len);const char *video=jstr(jget(v,"videoUrl")),*audio=jstr(jget(v,"audioUrl")),*title=jstr(jget(v,"title"));
 if(!rc&&(!video||!audio||strlen(video)>8192||strlen(audio)>8192||strncmp(video,"https://",8)||strncmp(audio,"https://",8)||strpbrk(video,"\r\n")||strpbrk(audio,"\r\n")))rc=fail("Resolver returned invalid media URLs");
 if(!rc){state_lock();if(!S->yt_active||strcmp(S->yt_token,token)){state_unlock();rc=fail("YouTube cancelled");}else{S->yt_duration=jnum(jget(v,"duration"),0);
 snprintf(S->yt_video,sizeof S->yt_video,"%s",video);snprintf(S->yt_audio,sizeof S->yt_audio,"%s",audio);generation=playback_begin_locked("",title?title:id,id,0,1);S->play_live=3;S->ui_return_source=0;state_unlock();char local[160];snprintf(local,sizeof local,"http://127.0.0.1:%d/youtube-stream/%s.ts",S->listen_port,token);rc=invoke_play_url(local);if(!rc){playback_setup_complete();for(double t=0;t<15&&yt_active(token);t+=0.1){state_lock();int claimed=S->yt_claimed;state_unlock();if(claimed)break;nap(0.1);}state_lock();if(!S->yt_claimed)rc=fail("Receiver did not request YouTube stream");else if(!S->yt_active)rc=fail("%s",S->yt_error[0]?S->yt_error:"YouTube stream failed");state_unlock();}}}
 jfree(v);free(result.p);if(rc){state_lock();snprintf(S->yt_error,sizeof S->yt_error,"%s",g_err);int started=S->playing&&S->play_live==3;if(started)playback_end_locked();else S->yt_active=0;state_unlock();if(started){receiver_stop();schedule_tv_return(1,POST_STOP_DISMISS);}return -1;}jf_log("YouTube playback started: %s (generation %lu)",title?title:id,generation);sb_fmt(out,"{\"playing\":true,\"source\":\"youtube\",\"generation\":%lu}",generation);return 0;}
#endif
static void yt_stream(int fd,const char *path,const char *method){if(strcmp(method,"GET")||strlen(path)!=35||strcmp(path+32,".ts")){send_json_error(fd,404,"Unknown YouTube stream",method);return;}char token[33],video[8193],audio[8193];memcpy(token,path,32);token[32]=0;state_lock();if(!S->yt_active||strcmp(S->yt_token,token)||S->yt_claimed){state_unlock();send_json_error(fd,404,"Expired YouTube stream",method);return;}S->yt_claimed=1;S->yt_claim_at=mono_now();snprintf(video,sizeof video,"%s",S->yt_video);snprintf(audio,sizeof audio,"%s",S->yt_audio);state_unlock();alarm(0);
 int in=-1;pid_t pid=yt_spawn(&in,"",video,audio,1);int rc=pid<0,started=0;double until=mono_now()+25;char buf[32768];struct timeval timeout={3,0};setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
 for(int attempt=0;attempt<2;attempt++){
 while(!rc&&yt_active(token)){if(mono_now()>until){rc=1;break;}struct pollfd p={in,POLLIN,0};int r=poll(&p,1,100);if(r<0&&errno==EINTR)continue;if(r<0){rc=1;break;}if(!r)continue;ssize_t n=read(in,buf,sizeof buf);if(n<=0){rc=n<0;break;}if(!started){const char *h="HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";rc=write_all_fd(fd,h,strlen(h));started=1;}if(!rc)rc=write_all_fd(fd,buf,n);until=mono_now()+25;}
 if(in>=0)close(in);int status=yt_reap(pid);fprintf(stderr,"YouTube packet-copy exit=%d signal=%d started=%d\n",status>=0&&WIFEXITED(status)?WEXITSTATUS(status):-1,status>=0&&WIFSIGNALED(status)?WTERMSIG(status):0,started);if(status<0||!WIFEXITED(status)||WEXITSTATUS(status))rc=1;
 if(started||attempt||!yt_active(token))break;
 /* Signed CDN URLs are ephemeral: one fresh resolution before any TS bytes. */
 char id[JF_ID_MAX];state_lock();snprintf(id,sizeof id,"%s",S->play_item);state_unlock();
 struct sb fresh={0};int retry=yt_resolve("play",id,"0",token,&fresh);
 struct jval *v=retry?NULL:json_parse(fresh.p,fresh.len);const char *vu=jstr(jget(v,"videoUrl")),*au=jstr(jget(v,"audioUrl"));
 if(retry||!vu||!au||strlen(vu)>8192||strlen(au)>8192||strncmp(vu,"https://",8)||strncmp(au,"https://",8)||strpbrk(vu,"\r\n")||strpbrk(au,"\r\n")){jfree(v);free(fresh.p);rc=1;break;}
 snprintf(video,sizeof video,"%s",vu);snprintf(audio,sizeof audio,"%s",au);jfree(v);free(fresh.p);
 in=-1;pid=yt_spawn(&in,"",video,audio,1);rc=pid<0;until=mono_now()+25;
 }
  if(started&&!rc){state_lock();double end=(S->yt_claim_at>0?S->yt_claim_at:S->play_started)+YT_STARTUP_GRACE+S->yt_duration+YT_DRAIN_GRACE;state_unlock();while(yt_active(token)&&mono_now()<end)nap(0.1);}
 state_lock();int owned=S->yt_active&&!strcmp(S->yt_token,token);if(owned){snprintf(S->yt_error,sizeof S->yt_error,"%s",!started||rc?"HTTPS or remux failure":"Playback ended");playback_end_locked();}state_unlock();if(owned){if(!started)send_json_error(fd,502,"HTTPS or remux failure",method);
#ifndef REBOX_YOUTUBE_MODULE
receiver_stop();schedule_tv_return(1,POST_STOP_DISMISS);
#endif
}}
static int yt_state(struct jval *payload,struct sb *out){char path[512];snprintf(path,sizeof path,"%s/state/youtube-state.json",persist_root);if(!payload){size_t n=0;char *raw=read_file(path,&n);if(raw&&n<=4096)sb_putn(out,raw,n);else sb_puts(out,"{}");free(raw);return 0;}struct sb b={0};const char *q=jstr(jget(payload,"query")),*id=jstr(jget(payload,"videoId"));char text[65];snprintf(text,sizeof text,"%.64s",q?q:"");int page=(int)jnum(jget(payload,"page"),0),focus=(int)jnum(jget(payload,"index"),0);if(page<0||page>100)page=0;if(focus<0||focus>5)focus=0;sb_puts(&b,"{\"query\":");sb_json_str(&b,text);sb_fmt(&b,",\"page\":%d,\"index\":%d,\"videoId\":",page,focus);sb_json_str(&b,yt_valid_id(id)?id:"");sb_puts(&b,"}");int rc=atomic_write(path,b.p,b.len,0600);free(b.p);if(rc)return fail("Unable to save YouTube state");sb_puts(out,"{\"saved\":true}");return 0;}

/* Native MENU watcher starts a due updater only while the receiver is idle.
 * Runtime/bytecode work lives entirely on-box and consumes no resident RAM. */
static void yt_maybe_update(void){
 if(S->yt_updater){int status;pid_t r=waitpid(S->yt_updater,&status,WNOHANG);if(r!=0)S->yt_updater=0;return;}
 double now=mono_now();if(now-S->yt_update_check<60)return;S->yt_update_check=now;
 state_lock();
#ifdef REBOX_YOUTUBE_MODULE
int busy=S->yt_active;
#else
int busy=S->playing||S->iptv_active||S->yt_active;
#endif
state_unlock();if(busy)return;
 char root[512],exe[512],script[512],stamp[512],home[512],ca[512];snprintf(root,sizeof root,"%s/youtube",persist_root);snprintf(exe,sizeof exe,"%s/bin/python",root);snprintf(script,sizeof script,"%s/updater.pyc",root);snprintf(stamp,sizeof stamp,"%s/update-status.json",root);snprintf(home,sizeof home,"%s/python",root);snprintf(ca,sizeof ca,"%s/iptv/ca-certificates.crt",persist_root);struct stat st;

#if defined(REBOX_YOUTUBE_MODULE) && !defined(REBOX_HOST_TEST)
 const char *package=getenv("REBOX_MODULE_PACKAGE");if(package){snprintf(script,sizeof script,"%s/bin/updater.pyc",package);snprintf(ca,sizeof ca,"%s/youtube/ca-certificates.crt",persist_root);}
#endif
 if(access(exe,X_OK)||access(script,R_OK))return;if(!stat(stamp,&st)&&time(NULL)-st.st_mtime<7*86400)return;
 pid_t owner=getpid(),pid=fork();if(!pid){setpgid(0,0);prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()!=owner)_exit(127);alarm(360);for(int i=0;i<1024;i++)close(i);int null=open("/dev/null",O_RDWR);dup2(null,0);dup2(null,1);dup2(null,2);if(null>2)close(null);setenv("PYTHONHOME",home,1);setenv("REBOX_YOUTUBE_RUNTIME",root,1);
#ifdef REBOX_YOUTUBE_MODULE
const char *package=getenv("REBOX_MODULE_PACKAGE");if(package){char fetch[1024];snprintf(fetch,sizeof fetch,"%s/bin/fetch",package);setenv("REBOX_YOUTUBE_FETCH",fetch,1);}
#endif
setenv("SSL_CERT_FILE",ca,1);execl(exe,exe,script,(char *)NULL);_exit(127);}if(pid>0)S->yt_updater=pid;
}
