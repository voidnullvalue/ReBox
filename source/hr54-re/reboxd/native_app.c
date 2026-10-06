#include "native_app.h"
#include "module_rpc.h"
#include "playback.h"
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static char owner[64],journal[REBOX_PATH_MAX];
static int persist(const char *id){struct sb b={0};sb_puts(&b,"{\"module\":");sb_json_str(&b,id);sb_puts(&b,"}");int rc=rb_atomic(journal,b.p,b.len);free(b.p);return rc;}
int rb_native_init(const ReboxRegistry *r){
    if(rb_path(journal,sizeof journal,r->root,"module-state/.native-owner.json",NULL))return -1;size_t n;char *raw=rb_read(journal,4096,&n);if(!raw)return errno==ENOENT?0:-1;
    struct jval *v=json_parse(raw,n);const char *id=jstr(jget(v,"module"));int bad=!id||(*id&&!rb_id(id));if(!bad)strcpy(owner,id);jfree(v);free(raw);return bad?fail("invalid native presentation journal"):0;
}
int rb_native_busy(const char *id){pthread_mutex_lock(&lock);int busy=*owner&&(!id||!strcmp(owner,id));pthread_mutex_unlock(&lock);return busy;}
void rb_native_system_json(struct sb *out){
    pthread_mutex_lock(&lock);sb_puts(out,"{\"ok\":true,\"ready\":true,\"frontend\":\"native\",\"nativeModule\":");sb_json_str(out,owner);sb_fmt(out,",\"mediaBusy\":%s}",*owner||rb_playback_busy(NULL)?"true":"false");pthread_mutex_unlock(&lock);
}
static int running(const RbReply *reply,int *value){struct jval *v=json_parse(reply->body.p,reply->body.len),*field=jget(v,"running");int valid=reply->status==200&&field&&(field->t==J_TRUE||field->t==J_FALSE);if(valid)*value=jbool(field,0);jfree(v);return valid?0:-1;}
static int clear(const char *id){pthread_mutex_lock(&lock);int rc=0;if(!strcmp(owner,id)){rc=persist("");if(!rc)owner[0]=0;}pthread_mutex_unlock(&lock);return rc;}
int rb_native_operation(ReboxModule *m,const char *operation,const char *body,struct sb *out){
    if(!m->installed)return 404;if(!m->enabled||!m->healthy||!m->native_app||m->kind!=REBOX_MODULE_NATIVE_APP)return 409;
    int start=!strcmp(operation,"start"),stop=!strcmp(operation,"stop"),status=!strcmp(operation,"status");if(!start&&!stop&&!status)return 404;
    struct jval *v=json_parse(body&&*body?body:"{}",body&&*body?strlen(body):2);int bad=!v||v->t!=J_OBJ||v->n;jfree(v);if(bad)return 400;
    if(start){if(rb_playback_busy(NULL))return 409;pthread_mutex_lock(&lock);if(*owner&&strcmp(owner,m->id)){pthread_mutex_unlock(&lock);return 409;}
        if(persist(m->id)){pthread_mutex_unlock(&lock);return 500;}strcpy(owner,m->id);pthread_mutex_unlock(&lock);
    }
    char path[64];snprintf(path,sizeof path,"/native/%s",operation);RbReply reply;if(rb_rpc(m,status?"GET":"POST",path,status?NULL:"{}",&reply,start?15:stop?8:2))return 502;
    int active=0,rc=reply.status;if(!running(&reply,&active)){if(active){pthread_mutex_lock(&lock);if(!*owner&&!persist(m->id))strcpy(owner,m->id);pthread_mutex_unlock(&lock);}else if(clear(m->id))rc=500;}
    else if(rc==200)rc=502;
    if(rc==200)sb_putn(out,reply.body.p,reply.body.len);rb_reply_free(&reply);return rc;
}
void rb_native_tick(const ReboxRegistry *r){
    char id[64];pthread_mutex_lock(&lock);strcpy(id,owner);pthread_mutex_unlock(&lock);
    for(size_t i=0;i<r->count;i++){const ReboxModule *m=&r->modules[i];if(*id&&strcmp(id,m->id))continue;if(!m->installed||!m->enabled||!m->healthy||!m->native_app)continue;
        RbReply reply;if(rb_rpc(m,"GET","/native/status",NULL,&reply,1))continue;int active;if(!running(&reply,&active)){
            if(*id&&!active)clear(id);
            else if(!*id&&active){pthread_mutex_lock(&lock);if(!persist(m->id))strcpy(owner,m->id);pthread_mutex_unlock(&lock);rb_reply_free(&reply);break;}
        }rb_reply_free(&reply);
    }
}
