#include "module_manager.h"
#include "module_manifest.h"
#include "module_process.h"
#include "module_auth.h"
#include "package.h"
#include "playback.h"
#include "native_app.h"
static void state_path(const ReboxRegistry *r,const char *id,char path[REBOX_PATH_MAX]){char file[80];snprintf(file,sizeof file,"%s.json",id);rb_path(path,REBOX_PATH_MAX,r->root,"module-state",file);}
static int copy_file(const char *from,const char *to,size_t cap){size_t n;char *p=rb_read(from,cap,&n);if(!p)return -1;int rc=rb_atomic(to,p,n);free(p);return rc;}
/* A single journal is sufficient: all mutations execute on the core reactor.
   An uncommitted promotion is rolled back before discovery becomes public. */
int rb_manager_recover(ReboxRegistry *r) {
    char journal[REBOX_PATH_MAX];rb_path(journal,sizeof journal,r->root,"module-state/transaction.json",NULL);size_t n;char *raw=rb_read(journal,4096,&n);
    if(!raw)return errno==ENOENT?0:-1;struct jval *v=json_parse(raw,n);free(raw);
    const char *id=jstr(jget(v,"id")),*stage=jstr(jget(v,"stage"));
    if(!rb_id(id)||!stage||strlen(stage)!=32||strspn(stage,"0123456789abcdef")!=32){jfree(v);return fail("invalid install recovery journal");}
    char live[REBOX_PATH_MAX],dir[REBOX_PATH_MAX],old[REBOX_PATH_MAX],state[REBOX_PATH_MAX],saved[REBOX_PATH_MAX];
    rb_path(live,sizeof live,r->root,"modules",id);rb_path(dir,sizeof dir,r->root,"staging",stage);rb_path(old,sizeof old,dir,"old",NULL);rb_path(saved,sizeof saved,dir,"state.old",NULL);state_path(r,id,state);
    struct stat st;int rc=0;if(!lstat(old,&st)){if(!S_ISDIR(st.st_mode)||rb_remove_tree(live)||rename(old,live))rc=-1;}
    else if(!jbool(jget(v,"hadOld"),0)&&rb_remove_tree(live))rc=-1;
    if(!rc){if(jbool(jget(v,"hadState"),0))rc=copy_file(saved,state,4096);else if(unlink(state)&&errno!=ENOENT)rc=-1;}
    if(!rc){rc=unlink(journal);if(!rc)rb_remove_tree(dir);rb_log(id,"transaction rolled back");}jfree(v);return rc;
}
static int transaction_begin(ReboxRegistry *r,const char *id,const char *stage,const char *dir,int had_old) {
    char state[REBOX_PATH_MAX],saved[REBOX_PATH_MAX],journal[REBOX_PATH_MAX];state_path(r,id,state);rb_path(saved,sizeof saved,dir,"state.old",NULL);rb_path(journal,sizeof journal,r->root,"module-state/transaction.json",NULL);
    struct stat st;int had_state=!lstat(state,&st);if(had_state&&copy_file(state,saved,4096))return -1;
    struct sb b={0};sb_fmt(&b,"{\"id\":\"%s\",\"stage\":\"%s\",\"hadOld\":%s,\"hadState\":%s}",id,stage,had_old?"true":"false",had_state?"true":"false");int rc=rb_atomic(journal,b.p,b.len);free(b.p);return rc;
}
static int transaction_commit(ReboxRegistry *r){char p[REBOX_PATH_MAX];rb_path(p,sizeof p,r->root,"module-state/transaction.json",NULL);if(unlink(p))return -1;rb_path(p,sizeof p,r->root,"module-state",NULL);int fd=open(p,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return -1;int rc=fsync(fd);close(fd);return rc;}
int rb_manager_install(ReboxRegistry *r,const char *url,const char *sha,const char *builtin,struct sb *out) {
    if(sha&&*sha&&(strlen(sha)!=64||strspn(sha,"0123456789abcdef")!=64))return 400;
    char name[33],dir[REBOX_PATH_MAX],archive[REBOX_PATH_MAX],unpacked[REBOX_PATH_MAX];if(rb_random(name,32))return 500;
    rb_path(dir,sizeof dir,r->root,"staging",name);if(rb_mkdir(dir))return 500;
    rb_path(archive,sizeof archive,dir,"package.rbox",NULL);rb_path(unpacked,sizeof unpacked,dir,"unpacked",NULL);int code=400;
    rb_log(NULL,"install requested");
    const char *package=builtin?builtin:archive;
    if(!builtin&&rb_download(url,archive)){code=502;goto done;}
    char digest[65];if(rb_package_sha(package,digest)){code=500;goto done;}rb_log(digest,"package SHA256");
    if(sha&&*sha&&strcmp(sha,digest)){code=400;goto done;}
    if(rb_mkdir(unpacked)||rb_package_extract(package,unpacked))goto done;
    ReboxModule next;int parsed=rb_manifest_file(unpacked,&next);if(parsed||rb_module_files(unpacked,&next))goto done;
    ReboxModule *previous=rb_registry_find(r,next.id);ReboxModule backup={0};
    if(previous){backup=*previous;if(previous->core&&strcmp(previous->sha256,digest)){code=403;goto done;}if(rb_playback_busy(next.id)||rb_native_busy(next.id)){code=409;goto done;}}
    else if(r->count==REBOX_MAX_MODULES){code=409;goto done;}
    next.core=previous&&previous->core;next.installed=1;next.enabled=previous&&previous->installed?previous->enabled:1;next.default_enabled=previous?previous->default_enabled:0;
    if(previous){memcpy(next.package,previous->package,sizeof next.package);memcpy(next.sha256,previous->sha256,sizeof next.sha256);}
    char live[REBOX_PATH_MAX],old[REBOX_PATH_MAX];rb_path(live,sizeof live,r->root,"modules",next.id);rb_path(old,sizeof old,dir,"old",NULL);
    if(transaction_begin(r,next.id,name,dir,previous&&previous->installed)){code=500;goto done;}
    if(previous&&rb_process_stop(previous)){code=409;goto rollback;}
    if(previous&&previous->installed&&rename(live,old)){code=500;goto rollback;}
    if(rename(unpacked,live)||rb_module_files(live,&next)){code=500;goto rollback;}
    /* Health-check even a disabled update; do not commit broken executables. */
    if(rb_process_start(r,&next,1)){code=502;goto rollback;}
    if(!next.enabled&&rb_process_stop(&next)){code=502;goto rollback;}
    if(rb_state_save(r,&next)||transaction_commit(r)){code=500;goto rollback;}
    if(previous)*previous=next;else r->modules[r->count++]=next;
    rb_log(next.id,"install succeeded");sb_puts(out,"{\"ok\":true,\"module\":");rb_module_json(&next,out);sb_puts(out,"}");code=200;goto done;
rollback:
    rb_process_stop(&next);
    if(rb_manager_recover(r)){rb_log(next.id,"rollback needs recovery");return 500;}
    if(previous){*previous=backup;previous->pid=0;previous->healthy=0;if(previous->enabled)rb_process_start(r,previous,0);}
    rb_log(next.id,"install rolled back");
done:rb_remove_tree(dir);if(code!=200)rb_log(NULL,"install failed");return code;
}
int rb_manager_mutate(ReboxRegistry *r,const char *id,const char *action,struct sb *out) {
    ReboxModule *m=rb_registry_find(r,id);if(!m)return 404;
    if(!strcmp(action,"reinstall")){if(!m->core)return 404;if(m->installed)return 409;char path[REBOX_PATH_MAX];rb_path(path,sizeof path,r->root,"builtin",m->package);return rb_manager_install(r,NULL,m->sha256,path,out);}
    if(!m->installed)return 404;if(rb_playback_busy(id)||rb_native_busy(id))return 409;
    if(!strcmp(action,"enable")){if(!m->compatible)return 409;if(rb_process_start(r,m,1))return 502;m->enabled=1;if(rb_state_save(r,m)){m->enabled=0;rb_process_stop(m);return 500;}rb_log(id,"enabled");}
    else if(!strcmp(action,"disable")){if(rb_process_stop(m))return 409;int was=m->enabled;m->enabled=0;if(rb_state_save(r,m)){m->enabled=was;if(was)rb_process_start(r,m,0);return 500;}rb_log(id,"disabled");}
    else if(!strcmp(action,"uninstall")) {
        if(rb_process_stop(m))return 409;
        char name[33],dir[REBOX_PATH_MAX],live[REBOX_PATH_MAX],old[REBOX_PATH_MAX];if(rb_random(name,32))return 500;rb_path(dir,sizeof dir,r->root,"staging",name);if(rb_mkdir(dir))return 500;
        rb_path(live,sizeof live,r->root,"modules",id);rb_path(old,sizeof old,dir,"old",NULL);
        if(transaction_begin(r,id,name,dir,1)||rename(live,old))return 500;
        int was=m->enabled;m->installed=m->enabled=m->healthy=0;
        char state[REBOX_PATH_MAX];state_path(r,id,state);int failed=m->core?rb_state_save(r,m):(unlink(state)&&errno!=ENOENT);
        if(failed||transaction_commit(r)){rb_manager_recover(r);m->installed=1;m->enabled=was;if(was)rb_process_start(r,m,0);return 500;}
        rb_log(id,"uninstalled; data preserved");rb_remove_tree(dir);
        if(!m->core){size_t i=(size_t)(m-r->modules);memmove(m,m+1,(r->count-i-1)*sizeof *m);r->count--;}
    }else return 404;
    return sb_puts(out,"{\"ok\":true}")?500:200;
}
