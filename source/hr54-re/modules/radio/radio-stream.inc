static int iptv_active(const char *token){state_lock();int ok=S->iptv_active&&!strcmp(token,S->iptv_token);state_unlock();return ok;}
static int radio_write(int fd,const void *bytes,size_t length,const char *token){const char *p=bytes;double deadline=mono_now()+30;
 while(length&&iptv_active(token)){ssize_t n=send(fd,p,length,MSG_DONTWAIT|MSG_NOSIGNAL);if(n>0){p+=n;length-=n;deadline=mono_now()+30;continue;}if(n<0&&errno==EINTR)continue;
 if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)&&mono_now()<deadline){struct pollfd writable={fd,POLLOUT,0};poll(&writable,1,100);continue;}return -1;}return length?-1:0;}
/* Test the candidate while the current station remains active. STOP cancels it. */
static int radio_probe(const char *url,const char *token){
 const char *root="/var/hr54-persist/rebox/module-data/youtube";char exe[1024],home[1024],ca[1024],script[1200];
 snprintf(exe,sizeof exe,"%s/youtube/bin/python",root);snprintf(home,sizeof home,"%s/youtube/python",root);snprintf(ca,sizeof ca,"%s/youtube/ca-certificates.crt",root);snprintf(script,sizeof script,"%s/bin/worker.py",package_root);
#ifdef REBOX_HOST_TEST
 snprintf(exe,sizeof exe,"%s/bin/python",package_root);
#endif
 pid_t owner=getpid(),pid=fork();if(pid<0)return fail("Unable to check station");
 if(!pid){setpgid(0,0);prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()!=owner)_exit(127);for(int i=3;i<1024;i++)close(i);setenv("PYTHONHOME",home,1);setenv("SSL_CERT_FILE",ca,1);setenv("PYTHONDONTWRITEBYTECODE","1",1);execl(exe,exe,script,"--probe",url,(char *)NULL);_exit(127);}setpgid(pid,pid);
 int status=0;double until=mono_now()+20;while(iptv_active(token)&&mono_now()<until){pid_t done=waitpid(pid,&status,WNOHANG);if(done==pid)return WIFEXITED(status)&&WEXITSTATUS(status)==0?0:fail("Next station is unavailable");if(done<0&&errno!=EINTR)break;nap(.02);}
 kill(-pid,SIGKILL);kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}return fail("Station check cancelled or timed out");
}
static void iptv_stream(int fd,const char *path,const char *method){
 if(strcmp(method,"GET")||strlen(path)!=35||strcmp(path+32,".ts")){rb_http_error(fd,404,"Unknown radio stream");return;}
 char token[33],url[8193];memcpy(token,path,32);token[32]=0;
 state_lock();if(!S->iptv_active||strcmp(token,S->iptv_token)||S->iptv_claimed){state_unlock();rb_http_error(fd,404,"Expired radio stream");return;}S->iptv_claimed=1;snprintf(url,sizeof url,"%s",S->iptv_url);state_unlock();
 const char *root="/var/hr54-persist/rebox/module-data/youtube";const char *test=getenv("REBOX_RADIO_RUNTIME");if(test)root=test;
 char exe[1024],script[1200],home[1024],ca[1024],mux[1200],black[1200];snprintf(exe,sizeof exe,"%s/youtube/bin/python",root);snprintf(home,sizeof home,"%s/youtube/python",root);snprintf(ca,sizeof ca,"%s/youtube/ca-certificates.crt",root);snprintf(script,sizeof script,"%s/bin/worker.py",package_root);snprintf(mux,sizeof mux,"%s/bin/audio-remux",package_root);snprintf(black,sizeof black,"%s/bin/black.ts",package_root);
#ifdef REBOX_HOST_TEST
 snprintf(exe,sizeof exe,"%s/bin/python",package_root);
#endif
 int p[2],rc=pipe(p),started=0;pid_t pid=-1,owner=getpid();
 if(!rc){pid=fork();if(!pid){setpgid(0,0);prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()!=owner)_exit(127);dup2(p[1],1);for(int i=3;i<1024;i++)close(i);setenv("PYTHONHOME",home,1);setenv("SSL_CERT_FILE",ca,1);setenv("JF_PERSIST_ROOT",root,1);setenv("REBOX_RADIO_MUX",mux,1);setenv("REBOX_RADIO_VIDEO",black,1);setenv("REBOX_RADIO_LIVE","1",1);setenv("PYTHONDONTWRITEBYTECODE","1",1);execl(exe,exe,script,url,(char *)NULL);_exit(127);}close(p[1]);if(pid<0){close(p[0]);rc=-1;}else setpgid(pid,pid);}
 struct timeval timeout={3,0};setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);double until=mono_now()+30;char buf[32768];
 while(!rc&&iptv_active(token)){if(mono_now()>until){rc=-1;break;}struct pollfd poller={p[0],POLLIN,0};int ready=poll(&poller,1,100);if(ready<0&&errno==EINTR)continue;if(ready<0){rc=-1;break;}if(!ready)continue;ssize_t n=read(p[0],buf,sizeof buf);if(n<=0){rc=-1;break;}
 if(!started){const char *h="HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";rc=radio_write(fd,h,strlen(h),token);started=1;}if(!rc)rc=radio_write(fd,buf,n,token);until=mono_now()+30;}
 if(pid>0){close(p[0]);kill(pid,SIGTERM);int status=0;pid_t done=0;double deadline=mono_now()+.5;
 while(mono_now()<deadline){done=waitpid(pid,&status,WNOHANG);if(done!=0)break;nap(.02);}
 if(!done){kill(-pid,SIGKILL);kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}}}
 state_lock();int owned=S->iptv_active&&!strcmp(token,S->iptv_token);if(owned){snprintf(S->iptv_error,sizeof S->iptv_error,"Radio stream ended or unavailable");playback_end_locked();}state_unlock();if(owned&&!started)rb_http_error(fd,502,"Radio stream unavailable");
}
