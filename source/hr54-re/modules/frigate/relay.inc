/* The module owns upstream/worker lifetime; core owns the receiver decoder. */
static int relay_path(char *out,size_t cap){
    const char *package=getenv("REBOX_MODULE_PACKAGE");
    return !package||snprintf(out,cap,"%s/bin/relay",package)>=(int)cap?-1:0;
}
static int stream_current(const char *token){
    pthread_mutex_lock(&state_mutex);int ok=playing&&!strcmp(session,token);pthread_mutex_unlock(&state_mutex);return ok;
}
static void serve_stream(int fd,const char *path,const char *method){
    if(strcmp(method,"GET")||strlen(path)!=35||strcmp(path+32,".ts")){rb_http_error(fd,404,"Unknown Frigate stream");return;}
    char token[33],stream[256],address[64],number[16],helper[1024];memcpy(token,path,32);token[32]=0;
    pthread_mutex_lock(&state_mutex);
    if(!playing||claimed||strcmp(session,token)){pthread_mutex_unlock(&state_mutex);rb_http_error(fd,404,"Expired Frigate stream");return;}
    claimed=1;strcpy(stream,selected_stream);strcpy(address,stream_host);snprintf(number,sizeof number,"%d",stream_port);pthread_mutex_unlock(&state_mutex);
    int pipefd[2],started=0,failed=1,status=0;pid_t child=-1;
    if(relay_path(helper,sizeof helper)||pipe(pipefd))goto done;
    pid_t owner=getpid();child=fork();
    if(!child){
        setpgid(0,0);prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()!=owner)_exit(127);
        dup2(pipefd[1],1);for(int i=3;i<1024;i++)close(i);
        execl(helper,helper,address,number,stream,(char *)NULL);_exit(127);
    }
    close(pipefd[1]);
    if(child<0){close(pipefd[0]);goto done;}setpgid(child,child);
    fprintf(stderr,"Frigate player relay path=%s pid=%ld output=MPEG-TS\n",helper,(long)child);
    struct timeval timeout={3,0};setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
    double deadline=mono_now()+25;unsigned char bytes[32768];failed=0;
    while(stream_current(token)){
        struct pollfd p={pipefd[0],POLLIN,0};int ready=poll(&p,1,100);
        if(ready<0&&errno==EINTR)continue;
        if(ready<0||mono_now()>deadline){failed=1;break;}if(!ready)continue;
        ssize_t n=read(pipefd[0],bytes,sizeof bytes);if(n<0&&errno==EINTR)continue;
        if(n<=0){failed=1;break;}
        if(!started){
            if(bytes[0]!=0x47){failed=1;break;}
            const char *head="HTTP/1.0 200 OK\r\nContent-Type: video/mp2t\r\nConnection: close\r\n\r\n";
            if(write_all_fd(fd,head,strlen(head))){failed=1;break;}started=1;
        }
        if(write_all_fd(fd,bytes,n)){failed=1;break;}deadline=mono_now()+25;
    }
    close(pipefd[0]);
    /* Reap a naturally exited worker first so its error code is retained.
     * The unreaped child PID/group cannot be reused before cleanup. */
    pid_t reaped=waitpid(child,&status,WNOHANG);
    if(!reaped){kill(-child,SIGKILL);kill(child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}}
    fprintf(stderr,"Frigate relay ended exit=%d signal=%d started=%d failed=%d\n",WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0,started,failed);
done:
    if(!started&&stream_current(token))rb_http_error(fd,502,"Frigate stream failed; check module log");
    pthread_mutex_lock(&state_mutex);if(!strcmp(session,token)&&playing){playing=0;snprintf(last_error,sizeof last_error,"%s",failed?"Frigate WebSocket or packet-copy stream failed; check module log":"");}pthread_mutex_unlock(&state_mutex);
}
