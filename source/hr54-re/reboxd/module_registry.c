#include "module_registry.h"
#include "module_manifest.h"
#include <dirent.h>
ReboxModule *rb_registry_find(ReboxRegistry *r,const char *id) {
    for(size_t i=0;i<r->count;i++)if(!strcmp(r->modules[i].id,id))return &r->modules[i];return NULL;
}
static int catalog(ReboxRegistry *r) {
    char path[REBOX_PATH_MAX];size_t size=0;rb_path(path,sizeof path,r->root,"builtin/catalog.json",NULL);
    char *raw=rb_read(path,REBOX_MANIFEST_MAX,&size);
    if(!raw)return errno==ENOENT?0:fail("catalog unavailable");
    struct jval *v=json_parse(raw,size);free(raw);struct jval *list=jget(v,"modules");
    if(!v||jnum(jget(v,"schema"),0)!=1||!list||list->t!=J_ARR||list->n>REBOX_MAX_MODULES){jfree(v);return fail("invalid builtin catalog");}
    for(size_t i=0;i<list->n;i++) {
        struct jval *it=list->items[i];const char *id=jstr(jget(it,"id")),*name=jstr(jget(it,"name"));
        const char *file=jstr(jget(it,"package")),*sha=jstr(jget(it,"sha256"));
        if(!rb_id(id)||rb_registry_find(r,id)||!file||!rb_relative(file)||strchr(file,'/')||strlen(file)>=128||
           !sha||strlen(sha)!=64||strspn(sha,"0123456789abcdef")!=64||!name||strlen(name)>=128){jfree(v);return fail("invalid builtin catalog entry");}
        ReboxModule *m=&r->modules[r->count++];snprintf(m->id,sizeof m->id,"%s",id);snprintf(m->name,sizeof m->name,"%s",name);
        snprintf(m->package,sizeof m->package,"%s",file);snprintf(m->sha256,sizeof m->sha256,"%s",sha);
        m->core=1;m->default_enabled=jbool(jget(it,"defaultEnabled"),1);m->home=1;
    }
    jfree(v);return 0;
}
int rb_state_save(const ReboxRegistry *r,const ReboxModule *m) {
    char path[REBOX_PATH_MAX],file[80];snprintf(file,sizeof file,"%s.json",m->id);rb_path(path,sizeof path,r->root,"module-state",file);
    struct sb b={0};sb_fmt(&b,"{\"schema\":1,\"enabled\":%s,\"installed\":%s,\"version\":",m->enabled?"true":"false",m->installed?"true":"false");
    sb_json_str(&b,m->version);sb_puts(&b,"}\n");int rc=rb_atomic(path,b.p,b.len);free(b.p);return rc;
}
static void merge_state(const ReboxRegistry *r,ReboxModule *m) {
    char path[REBOX_PATH_MAX],file[80];size_t size;snprintf(file,sizeof file,"%s.json",m->id);rb_path(path,sizeof path,r->root,"module-state",file);
    char *raw=rb_read(path,4096,&size);
    m->enabled=m->installed&&m->compatible&&m->default_enabled;
    if(!raw){if(errno!=ENOENT){m->enabled=0;snprintf(m->error,sizeof m->error,"state unreadable");}return;}
    struct jval *v=json_parse(raw,size);free(raw);struct jval *e=jget(v,"enabled");
    if(!v||jnum(jget(v,"schema"),0)!=1||!e||(e->t!=J_TRUE&&e->t!=J_FALSE)){m->enabled=0;snprintf(m->error,sizeof m->error,"invalid persisted state");}
    else m->enabled=m->installed&&m->compatible&&e->t==J_TRUE;
    jfree(v);
}
static int compare(const void *a,const void *b) {
    const ReboxModule *x=a,*y=b;if(x->order!=y->order)return x->order<y->order?-1:1;
    int n=strcmp(x->name,y->name);return n?n:strcmp(x->id,y->id);
}
int rb_registry_load(ReboxRegistry *r,const char *root) {
    memset(r,0,sizeof *r);
    if(!root||*root!='/'||strlen(root)>=sizeof r->root)return fail("invalid runtime root");
    snprintf(r->root,sizeof r->root,"%s",root);
    if(rb_mkdir(root))return fail("runtime root unavailable");
    const char *dirs[]={"modules","module-data","module-state","builtin","staging","log"};
    char path[REBOX_PATH_MAX];for(size_t i=0;i<sizeof dirs/sizeof *dirs;i++){rb_path(path,sizeof path,root,dirs[i],NULL);if(rb_mkdir(path))return fail("runtime directory unavailable");}
    /* Catalog failures do not authorize any built-in identity. Fail startup
       closed here rather than accidentally treating corrupt trust data as true. */
    if(catalog(r))return -1;
    rb_path(path,sizeof path,root,"modules",NULL);DIR *d=opendir(path);if(!d)return -1;
    struct dirent *e;
    while((e=readdir(d))) {
        if(!rb_id(e->d_name)||!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        ReboxModule *m=rb_registry_find(r,e->d_name);
        if(!m){if(r->count==REBOX_MAX_MODULES){rb_log(NULL,"module limit reached");continue;}m=&r->modules[r->count++];snprintf(m->id,sizeof m->id,"%s",e->d_name);snprintf(m->name,sizeof m->name,"%s",e->d_name);}
        char dir[REBOX_PATH_MAX];struct stat st;rb_path(dir,sizeof dir,root,"modules",e->d_name);ReboxModule parsed;
        m->installed=1;int rc=lstat(dir,&st)||!S_ISDIR(st.st_mode)?-1:rb_manifest_file(dir,&parsed);
        if(rc<0||(rc>=0&&strcmp(parsed.id,e->d_name))){snprintf(m->error,sizeof m->error,"invalid installed manifest or directory");continue;}
        parsed.core=m->core;parsed.default_enabled=m->default_enabled;parsed.installed=1;
        memcpy(parsed.package,m->package,sizeof parsed.package);memcpy(parsed.sha256,m->sha256,sizeof parsed.sha256);*m=parsed;
        if(rb_module_files(dir,m)){m->compatible=0;snprintf(m->error,sizeof m->error,"module files missing or unsafe");}
        rb_log(m->id,"discovered");
    }
    closedir(d);
    for(size_t i=0;i<r->count;i++)merge_state(r,&r->modules[i]);
    qsort(r->modules,r->count,sizeof *r->modules,compare);return 0;
}
#define BOOL(v) ((v)?"true":"false")
void rb_module_json(const ReboxModule *m,struct sb *b) {
    sb_puts(b,"{\"id\":");sb_json_str(b,m->id);sb_puts(b,",\"name\":");sb_json_str(b,m->name);
    sb_puts(b,",\"version\":");sb_json_str(b,m->version);sb_puts(b,",\"description\":");sb_json_str(b,*m->home_description?m->home_description:m->description);
    sb_fmt(b,",\"kind\":\"%s\",\"core\":%s,\"installed\":%s,\"enabled\":%s,\"compatible\":%s,\"healthy\":%s,\"home\":%s,\"order\":%d,\"restartCount\":%d,\"error\":",
        m->kind==REBOX_MODULE_NATIVE_APP?"native-app":"media",BOOL(m->core),BOOL(m->installed),BOOL(m->enabled),BOOL(m->compatible),BOOL(m->healthy),BOOL(m->home),m->order,m->restart_count);
    sb_json_str(b,m->error);sb_fmt(b,",\"capabilities\":{\"browse\":%s,\"search\":%s,\"settings\":%s,\"auth\":%s,\"playback\":%s,\"nativeApp\":%s},\"presentation\":{\"releaseSurface\":%s,\"releaseInput\":%s}}",
        BOOL(m->browse),BOOL(m->search),BOOL(m->settings),BOOL(m->auth),BOOL(m->playback),BOOL(m->native_app),BOOL(m->release_surface),BOOL(m->release_input));
}
int rb_registry_json(const ReboxRegistry *r,struct sb *b) {
    sb_puts(b,"{\"ok\":true,\"moduleApi\":1,\"modules\":[");for(size_t i=0;i<r->count;i++){if(i)sb_puts(b,",");rb_module_json(&r->modules[i],b);}return sb_puts(b,"]}");
}
