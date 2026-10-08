#include "modules.h"
static int field(const Json *j,int object,const char *key){return json_field(j,object,key);}
static int boolean(const Json *j,int object,const char *key){return json_bool(j,field(j,object,key));}
static long number(const Json *j,int object,const char *key){return json_number(j,field(j,object,key));}
static int string(const Json *j,int object,const char *key,char *out,size_t cap,int required){int t=field(j,object,key);if(t<0){out[0]=0;return required?-1:0;}return json_string(j,t,out,cap);}
static int open_response(const ApiResponse *r,Json *j){if(r->error!=API_OK||json_open(j,(const char *)r->bytes,r->length))return -1;int ok=field(j,0,"ok");if(j->t[0].type!='{'||(ok>=0&&!json_bool(j,ok))){json_close(j);return -1;}return 0;}
static int array(const Json *j,int object,const char *key,int max){int a=field(j,object,key);return a<0||j->t[a].type!='['||j->t[a].count>max?-1:a;}
int api_parse_modules(const ApiResponse *r,ModuleList *list){
    memset(list,0,sizeof *list);Json j;if(open_response(r,&j))return -1;int rc=-1,a=array(&j,0,"modules",REBOX_MAX_MODULES);if(a<0||number(&j,0,"moduleApi")!=1)goto done;
    for(int i=0;i<j.t[a].count;i++){
        int it=json_nth(&j,a,i);ModuleDescriptor *m=&list->items[i];char kind[24];
        if(string(&j,it,"id",m->id,sizeof m->id,1)||!api_module_id(m->id)||string(&j,it,"name",m->name,sizeof m->name,1)||!*m->name||
            string(&j,it,"version",m->version,sizeof m->version,0)||string(&j,it,"description",m->description,sizeof m->description,0)||
            string(&j,it,"error",m->error,sizeof m->error,0)||string(&j,it,"kind",kind,sizeof kind,0))goto done;
        for(int prior=0;prior<i;prior++)if(!strcmp(list->items[prior].id,m->id))goto done;
        if(*kind&&strcmp(kind,"media")&&strcmp(kind,"native-app"))goto done;
        m->native_app=!strcmp(kind,"native-app");m->installed=boolean(&j,it,"installed");m->enabled=boolean(&j,it,"enabled");m->healthy=boolean(&j,it,"healthy");m->core=boolean(&j,it,"core");m->order=number(&j,it,"order");m->home=field(&j,it,"home")<0||boolean(&j,it,"home");m->compatible=field(&j,it,"compatible")<0||boolean(&j,it,"compatible");
        int c=field(&j,it,"capabilities"),p=field(&j,it,"presentation");m->browse=boolean(&j,c,"browse");m->search=boolean(&j,c,"search");m->settings=boolean(&j,c,"settings");m->auth=boolean(&j,c,"auth");m->playback=boolean(&j,c,"playback");m->release_surface=boolean(&j,p,"releaseSurface");m->release_input=boolean(&j,p,"releaseInput");
        list->count++;
    }
    rc=0;
done:json_close(&j);if(rc)memset(list,0,sizeof *list);return rc;
}
int api_parse_media(const ApiResponse *r,MediaList *list){
    memset(list,0,sizeof *list);Json j;if(open_response(r,&j))return -1;int rc=-1,a=array(&j,0,"items",UI_PAGE_SIZE);if(a<0)goto done;
    list->total=number(&j,0,"total");list->offset=number(&j,0,"offset");list->has_more=boolean(&j,0,"hasMore");if(list->total<0||list->offset<0)goto done;
    for(int i=0;i<j.t[a].count;i++){
        int it=json_nth(&j,a,i);MediaItem *x=&list->item[i];
        if(string(&j,it,"id",x->id,sizeof x->id,1)||!*x->id||string(&j,it,"title",x->title,sizeof x->title,1)||!*x->title||
            string(&j,it,"kind",x->type,sizeof x->type,1)||string(&j,it,"subtitle",x->subtitle,sizeof x->subtitle,0)||
            string(&j,it,"description",x->overview,sizeof x->overview,0)||string(&j,it,"artwork",x->artwork,sizeof x->artwork,0))goto done;
        if(strcmp(x->type,"folder")&&strcmp(x->type,"item")&&strcmp(x->type,"action"))goto done;
        x->folder=!strcmp(x->type,"folder");x->playable=boolean(&j,it,"playable");x->year=number(&j,it,"year");x->duration=number(&j,it,"duration");x->resume=number(&j,it,"resume");list->count++;
    }
    if(list->has_more&&!list->count)goto done;rc=0;
done:json_close(&j);if(rc)memset(list,0,sizeof *list);return rc;
}
int api_parse_playback(const ApiResponse *r,PlaybackState *p){
    memset(p,0,sizeof *p);Json j;if(open_response(r,&j))return -1;int rc=field(&j,0,"playing")<0?-1:0;
    p->playing=boolean(&j,0,"playing");p->paused=boolean(&j,0,"paused");p->live=boolean(&j,0,"live");p->elapsed=number(&j,0,"elapsed");p->duration=number(&j,0,"duration");p->generation=(unsigned)number(&j,0,"generation");
    if(string(&j,0,"instance",p->instance,sizeof p->instance,0)||string(&j,0,"source",p->source,sizeof p->source,0)||string(&j,0,"title",p->title,sizeof p->title,0)||(p->playing&&!api_module_id(p->source)))rc=-1;
    int t=field(&j,0,"transport");p->can_stop=boolean(&j,t,"stop");p->can_pause=boolean(&j,t,"pause");p->can_resume=boolean(&j,t,"resume");p->can_seek=boolean(&j,t,"seek");p->can_channel_up=boolean(&j,t,"channelUp");p->can_channel_down=boolean(&j,t,"channelDown");json_close(&j);return rc;
}
static int scalar(const Json *j,int token,char *out,size_t cap,int *quoted){
    if(token<0)return -1;JsonToken t=j->t[token];*quoted=t.type=='"';if(*quoted)return json_string(j,token,out,cap);
    if(t.type!='0'&&t.type!='t'&&t.type!='f')return -1;size_t n=t.end-t.start;if(n>=cap)return -1;memcpy(out,j->text+t.start,n);out[n]=0;return 0;
}
int api_parse_settings(const ApiResponse *r,ModuleSettings *s){
    memset(s,0,sizeof *s);Json j;if(open_response(r,&j))return -1;int rc=-1,a=array(&j,0,"fields",MODULE_FIELDS);if(a<0&&field(&j,0,"fields")>=0)goto done;
    for(int i=0;a>=0&&i<j.t[a].count;i++){
        int it=json_nth(&j,a,i);ModuleField *f=&s->fields[i];char type[16];
        if(string(&j,it,"key",f->key,sizeof f->key,1)||string(&j,it,"label",f->label,sizeof f->label,1)||string(&j,it,"type",type,sizeof type,1)||scalar(&j,field(&j,it,"value"),f->value,sizeof f->value,&f->quoted))goto done;
        if(!strcmp(type,"bool"))f->type=FIELD_BOOL;else if(!strcmp(type,"integer"))f->type=FIELD_INTEGER;else if(!strcmp(type,"string"))f->type=FIELD_STRING;else if(!strcmp(type,"choice"))f->type=FIELD_CHOICE;else goto done;
        int choices=array(&j,it,"choices",MODULE_CHOICES);if(f->type==FIELD_CHOICE&&(choices<0||!j.t[choices].count))goto done;
        for(int k=0;choices>=0&&k<j.t[choices].count;k++){int v=json_nth(&j,choices,k);ModuleChoice *c=&f->choices[k];if(string(&j,v,"label",c->label,sizeof c->label,1)||scalar(&j,field(&j,v,"value"),c->value,sizeof c->value,&c->quoted))goto done;f->choice_count++;}
        s->field_count++;
    }
    a=array(&j,0,"actions",MODULE_ACTIONS);if(a<0&&field(&j,0,"actions")>=0)goto done;
    for(int i=0;a>=0&&i<j.t[a].count;i++){int it=json_nth(&j,a,i);ModuleAction *action=&s->actions[i];if(string(&j,it,"id",action->id,sizeof action->id,1)||!api_module_id(action->id)||string(&j,it,"label",action->label,sizeof action->label,1))goto done;s->action_count++;}
    rc=0;
done:json_close(&j);if(rc)memset(s,0,sizeof *s);return rc;
}
int api_parse_operation(const ApiResponse *r,OperationResult *out){
    memset(out,0,sizeof *out);Json j;if(open_response(r,&j))return -1;int rc=0;
    out->ready=boolean(&j,0,"ready");out->prepared=boolean(&j,0,"prepared");out->busy=field(&j,0,"mediaBusy")<0||boolean(&j,0,"mediaBusy");out->running=boolean(&j,0,"running");out->authenticated=boolean(&j,0,"authenticated");out->pending=boolean(&j,0,"pending");
    if(field(&j,0,"code")>=0&&j.t[field(&j,0,"code")].type!='n')rc=string(&j,0,"code",out->code,sizeof out->code,0);
    if(string(&j,0,"message",out->message,sizeof out->message,0)||string(&j,0,"pollAction",out->poll_action,sizeof out->poll_action,0)||string(&j,0,"nativeModule",out->native_module,sizeof out->native_module,0))rc=-1;
    if(r->kind==API_READY||r->kind==API_MODULE_OPEN){char frontend[16];if(string(&j,0,"frontend",frontend,sizeof frontend,1)||strcmp(frontend,"native")||!out->ready||field(&j,0,"nativeModule")<0)rc=-1;}
    if(r->kind==API_PREPARE&&!out->prepared)rc=-1;
    json_close(&j);return rc;
}
