#include "models.h"
static int fld(const Json *j,int i,const char *s){return json_field(j,i,s);}
static void str(const Json *j,int i,const char *s,char *d,size_t n){json_string(j,fld(j,i,s),d,n);}
/* The backend reports the active source's identity in /api/state. It is a
 * source-aware-stop hint; an unrecognized or absent string keeps -1. */
static int playback_source(const Json *j,int i){
    char name[32]={0};
    if(i<0||json_string(j,i,name,sizeof(name)))return -1;
    if(!strcmp(name,"jellyfin"))return 0;
    if(!strcmp(name,"iptv"))return 1;
    if(!strcmp(name,"frigate"))return 2;
    if(!strcmp(name,"youtube"))return 3;
    return -1;
}
int api_model(const ApiResponse *r,ApiModel *m){
    memset(m,0,sizeof(*m));if(r->error!=API_OK)return -1;Json j;if(json_open(&j,(const char *)r->bytes,r->length))return -1;
    if(j.t[0].type!='{'||j.t[0].count==0){json_close(&j);return -1;}int ok=fld(&j,0,"ok");if(ok>=0&&!json_bool(&j,ok)){json_close(&j);return -1;}
    m->ready=json_bool(&j,fld(&j,0,"ready"));m->authenticated=json_bool(&j,fld(&j,0,"authenticated"));m->pending=json_bool(&j,fld(&j,0,"pending"));m->running=json_bool(&j,fld(&j,0,"running"));m->native=json_bool(&j,fld(&j,0,"native"));str(&j,0,"code",m->code,sizeof(m->code));
    m->bitrate=(int)json_number(&j,fld(&j,fld(&j,0,"settings"),"jellyfinVideoBitrate"));
    int qualities=fld(&j,0,"qualityLevels");for(int i=0;i<8;i++){int option=json_nth(&j,qualities,i);if(option<0)break;int rate=(int)json_number(&j,fld(&j,option,"bitrate"));if(rate>0)m->quality_rates[m->quality_count++]=rate;}
    PlaybackState *p=&m->playback;p->playing=json_bool(&j,fld(&j,0,"playing"));p->paused=json_bool(&j,fld(&j,0,"paused"));p->live=json_bool(&j,fld(&j,0,"live"));p->elapsed=json_number(&j,fld(&j,0,"elapsed"));p->duration=json_number(&j,fld(&j,0,"duration"));p->generation=(unsigned)json_number(&j,fld(&j,0,"generation"));str(&j,0,"title",p->title,sizeof(p->title));str(&j,0,"source",p->source,sizeof(p->source));p->active_source=p->playing?(signed char)playback_source(&j,fld(&j,0,"source")):0;int transport=fld(&j,0,"transport");p->can_pause=json_bool(&j,fld(&j,transport,"pause"));p->can_resume=json_bool(&j,fld(&j,transport,"resume"));p->can_seek=json_bool(&j,fld(&j,transport,"seek"));p->can_stop=json_bool(&j,fld(&j,transport,"stop"));
    const char *key=NULL;switch(r->kind){case API_LIBRARIES:key="libraries";break;case API_ITEMS:case API_RESUME:key="items";break;case API_GROUPS:key="groups";break;case API_CHANNELS:key="channels";break;case API_CAMERAS:key="cameras";break;case API_SEARCH:key="results";break;default:break;}
    int rc=0;
    if(key){int a=fld(&j,0,key);if(a<0||j.t[a].type!='[')rc=-1;else{MediaList *l=&m->list;l->total=(int)json_number(&j,fld(&j,0,r->kind==API_GROUPS?"groupCount":"total"));l->has_more=json_bool(&j,fld(&j,0,"hasMore"));for(int n=0;n<UI_PAGE_SIZE;n++){int it=json_nth(&j,a,n);if(it<0)break;if(j.t[it].type!='{'){rc=-1;break;}MediaItem *x=&l->item[l->count++];str(&j,it,"id",x->id,sizeof(x->id));str(&j,it,r->kind==API_SEARCH?"title":"name",x->title,sizeof(x->title));if(r->kind==API_GROUPS){str(&j,it,"name",x->id,sizeof(x->id));str(&j,it,"label",x->title,sizeof(x->title));if(!*x->title)str(&j,it,"name",x->title,sizeof(x->title));}str(&j,it,"overview",x->overview,sizeof(x->overview));str(&j,it,"type",x->type,sizeof(x->type));str(&j,it,r->kind==API_SEARCH?"channel":r->kind==API_CAMERAS?"reason":"group",x->subtitle,sizeof(x->subtitle));x->folder=json_bool(&j,fld(&j,it,"isFolder"))||r->kind==API_LIBRARIES||r->kind==API_GROUPS;x->playable=json_bool(&j,fld(&j,it,"playable"))||r->kind==API_CHANNELS||r->kind==API_SEARCH;x->year=json_number(&j,fld(&j,it,"year"));x->resume=json_number(&j,fld(&j,it,"resumeSeconds"));x->duration=json_number(&j,fld(&j,it,"duration"));if(!x->duration){int rt=fld(&j,it,"runtime");if(rt>=0){uint64_t ticks=0;for(int k=j.t[rt].start;k<j.t[rt].end;k++){char c=j.text[k];if(c<'0'||c>'9')break;if(ticks>UINT64_MAX/10){ticks=0;break;}ticks=ticks*10+c-'0';}x->duration=(long)(ticks/10000000);}}if(!*x->title){rc=-1;break;}}if(l->total<l->count)l->total=l->count;}}
    if(r->kind==API_READY){char mode[24];str(&j,0,"frontend",mode,sizeof(mode));int doom=fld(&j,0,"doomRunning"),busy=fld(&j,0,"mediaBusy");m->running=json_bool(&j,doom);m->busy=busy<0||json_bool(&j,busy);if(!m->ready||strcmp(mode,"native")||doom<0)rc=-1;}
    if(r->kind==API_PREPARE){int prepared=fld(&j,0,"prepared");m->prepared=json_bool(&j,prepared);if(prepared<0||!m->prepared)rc=-1;}
    if(r->kind==API_STATE&&fld(&j,0,"playing")<0)rc=-1;
    json_close(&j);return rc;
}
