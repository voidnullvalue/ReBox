#include "../shared/sdk.h"
#include <sys/file.h>
#include <sys/wait.h>
#include "../../tools/native-ui/presentation_policy.h"
#define REBOX_DOOM_MODULE 1
static const char *persist_root;static const int native_frontend=1;static char package_engine[1024],package_wad[1024];
static pthread_mutex_t operation_mutex=PTHREAD_MUTEX_INITIALIZER;
static void jf_log(const char *fmt,...){va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);fputc('\n',stderr);va_end(ap);}
#include "lifecycle.inc"
#include "input.inc"
static void status(struct sb *out){char bin[1024],wad[1024];doom_path(bin,sizeof bin,"bin/hr54-doom-native");doom_path(wad,sizeof wad,"data/doom1.wad");sb_fmt(out,"{\"ok\":true,\"moduleApi\":1,\"available\":%s,\"armed\":%s,\"running\":%s,\"pid\":%d}",!access(bin,X_OK)&&!access(wad,R_OK)?"true":"false",doom_armed()?"true":"false",native_doom_busy()?"true":"false",doom_running_pid());}
static void handle(int fd,const RbRequest *request){
    /* Readiness must stay independent of native assets and execution. */
    if(!strcmp(request->path,"/status")||!strcmp(request->path,"/native/status")){struct sb out={0};status(&out);rb_http_json(fd,200,out.p);free(out.p);return;}
    pthread_mutex_lock(&operation_mutex);struct sb out={0};struct jval *body=json_parse(*request->body?request->body:"{}",*request->body?request->length:2);int rc=0,code=200;
    if(!body||body->t!=J_OBJ)code=400;
    else if(!strcmp(request->path,"/legacy/input")){
        if(!strcmp(request->method,"POST")){double keys=jnum(jget(body,"keys"),0);if(!doom_armed()||keys<0||keys>255||keys!=(int)keys)code=409;else doom_input_write((unsigned)keys);}
        sb_fmt(&out,"{\"ok\":true,\"keys\":%u}",doom_input_read());
    }
    else if(!strcmp(request->path,"/legacy/caps"))sb_puts(&out,"{\"ok\":true}");
    else if(body->n)code=400;
    else if(!strcmp(request->path,"/native/start"))rc=native_doom_start(&out);
    else if(!strcmp(request->path,"/native/stop")){doom_stop("module control");if(native_doom_busy())code=409;else sb_puts(&out,"{\"ok\":true,\"running\":false}");}
    else code=404;
    if(rc)rb_http_error(fd,502,g_err);else if(code!=200)rb_http_error(fd,code,"native lifecycle unavailable");else rb_http_json(fd,200,out.p?out.p:"{}");jfree(body);free(out.p);pthread_mutex_unlock(&operation_mutex);
}
int main(int argc,char **argv){(void)argc;persist_root=getenv("REBOX_MODULE_DATA");if(!persist_root)return 2;
#ifndef REBOX_HOST_TEST
    struct stat legacy;if(!lstat("/var/hr54-persist/jellyfin/doom",&legacy)&&S_ISDIR(legacy.st_mode)&&legacy.st_uid==geteuid())persist_root="/var/hr54-persist/jellyfin";
#endif
    if(rb_module_seed("default-data",""))return 1;
    if(getenv("REBOX_MODULE_SEED_ONLY"))return 0;
    char path[1024];snprintf(path,sizeof path,"%s/doom",persist_root);if(rb_mkdir(path))return 1;snprintf(path,sizeof path,"%s/doom/data",persist_root);if(rb_mkdir(path))return 1;
    char executable[1024];if(realpath(argv[0],executable)){char *slash=strrchr(executable,'/');if(slash){*slash=0;snprintf(package_engine,sizeof package_engine,"%s/hr54-doom-native",executable);if(access(package_engine,X_OK))package_engine[0]=0;}}
    const char *package=getenv("REBOX_MODULE_PACKAGE");if(package){snprintf(package_wad,sizeof package_wad,"%s/default-data/doom/data/doom1.wad",package);char legacy_wad[1024];snprintf(legacy_wad,sizeof legacy_wad,"%s/doom/data/doom1.wad",persist_root);if(!access(legacy_wad,R_OK)||access(package_wad,R_OK))package_wad[0]=0;}
    signal(SIGCHLD,SIG_IGN);int result=rb_module_serve(handle);doom_stop("module shutdown");return result;
}
