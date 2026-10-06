#include "module_process.h"
#include "module_rpc.h"
#include <sys/prctl.h>
static char sockets[80];
int rb_process_init(const char *path){if(strlen(path)>=sizeof sockets||rb_mkdir(path))return -1;strcpy(sockets,path);return 0;}
static int identity(const ReboxModule *m) {
    if(m->pid<=1)return 0;char proc[64],path[REBOX_PATH_MAX];snprintf(proc,sizeof proc,"/proc/%ld/exe",(long)m->pid);
    ssize_t n=readlink(proc,path,sizeof path-1);if(n<0)return 0;path[n]=0;return !strcmp(path,m->executable);
}
int rb_process_stop(ReboxModule *m) {
    if(m->pid>1){
        int status;pid_t rc=waitpid(m->pid,&status,WNOHANG);
        if(rc==0){
            if(!identity(m))return fail("module process identity mismatch");
            kill(m->pid,SIGTERM);double until=mono_now()+3;
            do{rc=waitpid(m->pid,&status,WNOHANG);if(rc!=0)break;nap(.02);}while(mono_now()<until);
            if(rc==0){if(!identity(m))return fail("module process identity mismatch");kill(m->pid,SIGKILL);while(waitpid(m->pid,&status,0)<0&&errno==EINTR){}}
        }
        m->pid=0;rb_log(m->id,"process stopped");
    }
    m->healthy=0;if(*m->socket)unlink(m->socket);return 0;
}
int rb_process_start(ReboxRegistry *r,ReboxModule *m,int manual) {
    if(!m->installed||!m->compatible)return fail("module not installed or incompatible");
    if(m->pid)return m->healthy?0:-1;if(manual)m->restart_count=0;
    char data[REBOX_PATH_MAX],log[REBOX_PATH_MAX],logfile[80];rb_path(data,sizeof data,r->root,"module-data",m->id);
    if(rb_mkdir(data))return fail("module data unavailable");
    if(snprintf(m->socket,sizeof m->socket,"%s/%s.sock",sockets,m->id)>=(int)sizeof m->socket)return fail("module socket path exceeds limit");
    unlink(m->socket);snprintf(logfile,sizeof logfile,"%s.log",m->id);rb_path(log,sizeof log,r->root,"log",logfile);
    int output=open(log,O_WRONLY|O_APPEND|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);if(output<0)return fail("module log unavailable");
    char eid[100],esock[160],edata[REBOX_PATH_MAX+24];
    snprintf(eid,sizeof eid,"REBOX_MODULE_ID=%s",m->id);snprintf(esock,sizeof esock,"REBOX_MODULE_SOCKET=%s",m->socket);snprintf(edata,sizeof edata,"REBOX_MODULE_DATA=%s",data);
    char *env[]={eid,esock,edata,"REBOX_MODULE_API=1","PATH=/bin:/usr/bin",NULL};char *argv[]={m->executable,NULL};
    pid_t owner=getpid(),pid=fork();
    if(!pid){prctl(PR_SET_PDEATHSIG,SIGTERM);if(getppid()!=owner)_exit(127);dup2(output,1);dup2(output,2);int null=open("/dev/null",O_RDONLY);if(null>=0)dup2(null,0);for(int i=3;i<65536;i++)close(i);execve(m->executable,argv,env);_exit(127);}
    close(output);if(pid<0)return fail("module launch failed");m->pid=pid;m->started=mono_now();
    double deadline=m->started+4;
    while(mono_now()<deadline){int status;pid_t exitpid=waitpid(pid,&status,WNOHANG);if(exitpid==pid){m->pid=0;break;}
        if(identity(m)&&!rb_rpc_health(m)){m->healthy=1;m->error[0]=0;rb_log(m->id,"process started");return 0;}nap(.05);}
    rb_process_stop(m);snprintf(m->error,sizeof m->error,"module failed startup health check");rb_log(m->id,"startup failed");return fail("module startup failed");
}
void rb_process_tick(ReboxRegistry *r) {
    for(size_t i=0;i<r->count;i++){ReboxModule *m=&r->modules[i];
        if(m->pid){int status;pid_t got=waitpid(m->pid,&status,WNOHANG);if(got==m->pid){m->pid=0;m->healthy=0;m->restart_count++;m->started=mono_now();snprintf(m->error,sizeof m->error,"module process exited");unlink(m->socket);rb_log(m->id,"process exited");}}
        if(m->enabled&&!m->pid&&m->restart_count<3&&mono_now()-m->started>=m->restart_count+1){if(rb_process_start(r,m,0))m->restart_count++;}
    }
}
void rb_process_shutdown(ReboxRegistry *r){for(size_t i=0;i<r->count;i++)rb_process_stop(&r->modules[i]);}
