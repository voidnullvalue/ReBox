#include "module_manifest.h"
static int text_field(struct jval *v,const char *key,char *out,size_t cap,int required) {
    struct jval *field=jget(v,key); const char *s=jstr(field);
    if(!field&&!required){out[0]=0;return 0;}
    if(!s||strlen(s)>=cap||(required&&!*s))return fail("invalid manifest string: %s",key);
    for(const char *p=s;*p;p++)if((unsigned char)*p<32)return fail("invalid manifest control character");
    memcpy(out,s,strlen(s)+1);return 0;
}
static int boolean(struct jval *v,const char *key,int *out) {
    struct jval *f=jget(v,key);if(f&&f->t!=J_TRUE&&f->t!=J_FALSE)return fail("invalid manifest boolean");
    *out=f&&f->t==J_TRUE;return 0;
}
int rb_manifest(const char *raw,size_t size,ReboxModule *m) {
    memset(m,0,sizeof *m);m->home=1;
    if(size>REBOX_MANIFEST_MAX)return fail("manifest exceeds limit");
    struct jval *v=json_parse(raw,size);int rc=-1;
    if(!v||v->t!=J_OBJ){fail("invalid JSON manifest");goto done;}
    if(text_field(v,"id",m->id,sizeof m->id,1)||!rb_id(m->id)){fail("invalid module ID");goto done;}
    if(text_field(v,"name",m->name,sizeof m->name,1)||text_field(v,"version",m->version,sizeof m->version,1)||
       text_field(v,"description",m->description,sizeof m->description,0)||text_field(v,"entrypoint",m->entrypoint,sizeof m->entrypoint,1)||
       text_field(v,"icon",m->icon,sizeof m->icon,0))goto done;
    if(!rb_relative(m->entrypoint)||(*m->icon&&!rb_relative(m->icon))){fail("invalid package-relative path");goto done;}
    const char *kind=jstr(jget(v,"kind"));
    if(kind&&!strcmp(kind,"media"))m->kind=REBOX_MODULE_MEDIA;
    else if(kind&&!strcmp(kind,"native-app"))m->kind=REBOX_MODULE_NATIVE_APP;
    else{fail("invalid module kind");goto done;}
    struct jval *caps=jget(v,"capabilities"),*ui=jget(v,"ui"),*pres=jget(v,"presentation");
    if(!caps||caps->t!=J_OBJ||(ui&&ui->t!=J_OBJ)||(pres&&pres->t!=J_OBJ)){fail("invalid module metadata");goto done;}
    if(boolean(caps,"browse",&m->browse)||boolean(caps,"search",&m->search)||boolean(caps,"settings",&m->settings)||
       boolean(caps,"auth",&m->auth)||boolean(caps,"playback",&m->playback)||boolean(caps,"nativeApp",&m->native_app)||
       boolean(pres,"releaseSurface",&m->release_surface)||boolean(pres,"releaseInput",&m->release_input))goto done;
    if(m->kind==REBOX_MODULE_NATIVE_APP?!m->native_app:m->native_app){fail("kind/capability mismatch");goto done;}
    if(text_field(ui,"homeDescription",m->home_description,sizeof m->home_description,0))goto done;
    double order=jnum(jget(ui,"order"),0);if(order< -100000||order>100000||order!=(int)order){fail("invalid UI order");goto done;}m->order=(int)order;
    if(jget(ui,"home")&&boolean(ui,"home",&m->home))goto done;
    if(jnum(jget(v,"schema"),-1)!=1||jnum(jget(v,"moduleApi"),-1)!=REBOX_API||jnum(jget(v,"minReboxApi"),-1)!=REBOX_API){
        snprintf(m->error,sizeof m->error,"unsupported schema or module API");rc=1;goto done;
    }
    m->compatible=1;rc=0;
done:jfree(v);return rc;
}
int rb_manifest_file(const char *dir,ReboxModule *m) {
    char path[REBOX_PATH_MAX];size_t n;
    if(rb_path(path,sizeof path,dir,"module.json",NULL))return -1;
    char *raw=rb_read(path,REBOX_MANIFEST_MAX,&n);if(!raw)return fail("manifest missing or exceeds limit");
    int rc=rb_manifest(raw,n,m);free(raw);return rc;
}
/* Refuse all symlink components, including ones which resolve inside the root.
 * The archive ABI deliberately supports regular files and directories only. */
static int regular_under(const char *dir,const char *rel,int executable) {
    char path[REBOX_PATH_MAX];struct stat st;
    if(rb_path(path,sizeof path,dir,rel,NULL))return -1;
    size_t begin=strlen(dir)+1;
    for(char *p=path+begin;*p;p++)if(*p=='/'){*p=0;int bad=lstat(path,&st)||!S_ISDIR(st.st_mode);*p='/';if(bad)return -1;}
    return lstat(path,&st)||!S_ISREG(st.st_mode)||(executable&&!(st.st_mode&S_IXUSR))?-1:0;
}
int rb_module_files(const char *dir,ReboxModule *m) {
    if(regular_under(dir,m->entrypoint,1))return fail("module executable missing or unsafe");
    if(*m->icon&&regular_under(dir,m->icon,0))return fail("module icon missing or unsafe");
    return rb_path(m->executable,sizeof m->executable,dir,m->entrypoint,NULL);
}
