/* Module-owned cached, bounded M3U library. */
#include <stdint.h>
#define IPTV_MAX_CHANNELS 10000
#define IPTV_URL_CAP 8192
struct iptv_channel { char id[32]; char *name,*tvg,*logo,*group,*ua,*ref,*url; };
static struct iptv_channel *iptv_channels;
static size_t iptv_count;
static struct stat iptv_stat;
static int iptv_loaded;
static char iptv_library_error[256];
static char *iptv_trim(char *s){while(isspace((unsigned char)*s))s++;size_t n=strlen(s);while(n&&isspace((unsigned char)s[n-1]))s[--n]=0;return s;}
/* Attribute/name delimiter: commas inside quotes are metadata. */
static char *iptv_comma(char *s){int quote=0;for(;*s;s++){if(*s=='"')quote=!quote;else if(*s==','&&!quote)return s;}return NULL;}
static char *iptv_attr(const char *s,const char *key){
 size_t k=strlen(key);while(*s){while(*s&&(*s==','||isspace((unsigned char)*s)))s++;const char *a=s;while(*s&&*s!='='&&*s!=','&&!isspace((unsigned char)*s))s++;size_t n=s-a;
 if(*s!='='){while(*s&&*s!=','&&!isspace((unsigned char)*s))s++;continue;}s++;int q=*s=='"';if(q)s++;const char *v=s;while(*s&&(q?*s!='"':(*s!=','&&!isspace((unsigned char)*s))))s++;
 if(n==k&&!strncmp(a,key,k)){size_t len=s-v;if(len>IPTV_URL_CAP)return NULL;char *r=malloc(len+1);if(r){memcpy(r,v,len);r[len]=0;}return r;}if(q&&*s)s++;}return strdup("");
}
static void iptv_free_channel(struct iptv_channel *c){free(c->name);free(c->tvg);free(c->logo);free(c->group);free(c->ua);free(c->ref);free(c->url);memset(c,0,sizeof *c);}
static uint64_t iptv_hash(uint64_t h,const char *s){for(;*s;s++){h^=(unsigned char)*s;h*=UINT64_C(1099511628211);}return h;}
static int iptv_parse_file(const char *path,struct iptv_channel **out,size_t *count){
 FILE *f=fopen(path,"r");if(!f)return fail("IPTV playlist unavailable");struct stat st;if(fstat(fileno(f),&st)||st.st_size>2*1024*1024){fclose(f);return fail("IPTV playlist exceeds 2 MiB");}
 struct iptv_channel *list=calloc(IPTV_MAX_CHANNELS,sizeof *list),c={0};size_t used=0;char line[16384];int rc=0;
 if(!list){fclose(f);return fail("out of memory");}
 while(fgets(line,sizeof line,f)){if(!strchr(line,'\n')&&!feof(f)){rc=fail("M3U line too long");break;}char *p=iptv_trim(line);
 if(!strncmp(p,"#EXTINF:",8)){iptv_free_channel(&c);char *comma=iptv_comma(p+8);if(!comma){rc=fail("Malformed EXTINF");break;}*comma=0;c.name=strdup(iptv_trim(comma+1));c.tvg=iptv_attr(p+8,"tvg-id");c.logo=iptv_attr(p+8,"tvg-logo");c.group=iptv_attr(p+8,"group-title");c.ua=iptv_attr(p+8,"http-user-agent");c.ref=iptv_attr(p+8,"http-referrer");if(!c.name||!c.tvg||!c.logo||!c.group||!c.ua||!c.ref){rc=fail("Invalid or oversized M3U metadata");break;}}
 else if(c.name&&!strncmp(p,"#EXTVLCOPT:",11)){char **v=NULL;if(!strncmp(p+11,"http-user-agent=",16))v=&c.ua;else if(!strncmp(p+11,"http-referrer=",14))v=&c.ref;if(v){char *value=strchr(p,'=')+1;if(strlen(value)>2048){rc=fail("M3U header too long");break;}free(*v);*v=strdup(value);if(!*v){rc=fail("out of memory");break;}}}
 else if(*p&&*p!='#'&&c.name){if(strlen(p)>IPTV_URL_CAP||strlen(c.ua)>2048||strlen(c.ref)>2048||strpbrk(c.ua,"\r\n")||strpbrk(c.ref,"\r\n")||used==IPTV_MAX_CHANNELS){rc=fail("M3U entry exceeds bounds");break;}c.url=strdup(p);if(!c.url){rc=fail("out of memory");break;}uint64_t h=iptv_hash(iptv_hash(UINT64_C(14695981039346656037),c.tvg),"\n");h=iptv_hash(h,c.url);snprintf(c.id,sizeof c.id,"ch-%016llx",(unsigned long long)h);list[used++]=c;memset(&c,0,sizeof c);}}
 if(ferror(f))rc=fail("Error reading M3U");fclose(f);iptv_free_channel(&c);if(rc){for(size_t i=0;i<used;i++)iptv_free_channel(&list[i]);free(list);return -1;}*out=list;*count=used;return 0;
}
/* Call under the module operation mutex; stream workers copy their URLs. */
static void iptv_reload(void){char path[512];snprintf(path,sizeof path,"%s/iptv/eng.m3u",persist_root);struct stat st;
 if(stat(path,&st)){if(!iptv_loaded)snprintf(iptv_library_error,sizeof iptv_library_error,"IPTV playlist unavailable");return;}
 if(iptv_loaded&&st.st_mtime==iptv_stat.st_mtime&&st.st_mtim.tv_nsec==iptv_stat.st_mtim.tv_nsec&&st.st_size==iptv_stat.st_size&&st.st_ino==iptv_stat.st_ino)return;
 struct iptv_channel *next=NULL;size_t n=0;if(iptv_parse_file(path,&next,&n)){snprintf(iptv_library_error,sizeof iptv_library_error,"%s",g_err);return;}
 for(size_t i=0;i<iptv_count;i++)iptv_free_channel(&iptv_channels[i]);free(iptv_channels);iptv_channels=next;iptv_count=n;iptv_stat=st;iptv_loaded=1;iptv_library_error[0]=0;jf_log("IPTV library loaded: %zu channels",n);
}
static struct iptv_channel *iptv_find(const char *id){if(!id||strlen(id)!=19)return NULL;for(size_t i=0;i<iptv_count;i++)if(!strcmp(id,iptv_channels[i].id))return &iptv_channels[i];return NULL;}
static int iptv_contains(const char *s,const char *q){size_t n=strlen(q);for(;*s;s++)if(!strncasecmp(s,q,n))return 1;return !n;}
static int iptv_library_api(const char *route,const char *query,struct sb *out){
 if(!iptv_loaded)return fail("%s",iptv_library_error);
 if(!strcmp(route,"/api/iptv/groups")){
  char number[32]="";query_param(query,"offset",number,sizeof number);long offset=strtol(number,NULL,10);if(offset<0)offset=0;if(offset>IPTV_MAX_CHANNELS)offset=IPTV_MAX_CHANNELS;
  number[0]=0;query_param(query,"limit",number,sizeof number);long limit=*number?strtol(number,NULL,10):IPTV_MAX_CHANNELS;if(limit<1)limit=1;if(limit>IPTV_MAX_CHANNELS)limit=IPTV_MAX_CHANNELS;
  sb_puts(out,"{\"groups\":[");int emitted=0;size_t groups=0;
  for(size_t i=0;i<iptv_count;i++){const char *g=iptv_channels[i].group;int seen=0;for(size_t j=0;j<i;j++)if(!strcmp(g,iptv_channels[j].group)){seen=1;break;}if(seen)continue;
   size_t at=groups++;if(at<(size_t)offset||emitted>=limit)continue;size_t count=0;for(size_t j=i;j<iptv_count;j++)if(!strcmp(g,iptv_channels[j].group))count++;
   if(emitted++)sb_puts(out,",");sb_puts(out,"{\"name\":");sb_json_str(out,g);sb_puts(out,",\"label\":");sb_json_str(out,*g?g:"Other channels");sb_fmt(out,",\"count\":%zu}",count);
  }
  sb_fmt(out,"],\"total\":%zu,\"groupCount\":%zu,\"offset\":%ld,\"limit\":%ld,\"hasMore\":%s}",iptv_count,groups,offset,limit,(size_t)(offset+emitted)<groups?"true":"false");return 0;
 }
 char group[IPTV_URL_CAP]="",q[256]="",num[32]="";int has_group=query_param(query,"group",group,sizeof group);query_param(query,"query",q,sizeof q);query_param(query,"offset",num,sizeof num);long offset=strtol(num,NULL,10);if(offset<0)offset=0;if(offset>IPTV_MAX_CHANNELS)offset=IPTV_MAX_CHANNELS;num[0]=0;query_param(query,"limit",num,sizeof num);long limit=*num?strtol(num,NULL,10):6;if(limit<1)limit=1;if(limit>60)limit=60;
 size_t total=0;long emitted=0;sb_puts(out,"{\"channels\":[");for(size_t i=0;i<iptv_count;i++){struct iptv_channel *c=&iptv_channels[i];if(has_group&&strcmp(c->group,group))continue;if(*q&&!iptv_contains(c->name,q)&&!iptv_contains(c->tvg,q)&&!iptv_contains(c->group,q))continue;size_t at=total++;if(at<(size_t)offset||emitted>=limit)continue;if(emitted++)sb_puts(out,",");sb_puts(out,"{\"id\":");sb_json_str(out,c->id);sb_puts(out,",\"name\":");sb_json_str(out,c->name);sb_puts(out,",\"tvgId\":");sb_json_str(out,c->tvg);sb_puts(out,",\"group\":");sb_json_str(out,c->group);sb_puts(out,",\"logo\":");sb_json_str(out,c->logo);sb_puts(out,"}");}sb_fmt(out,"],\"total\":%zu,\"offset\":%ld,\"limit\":%ld,\"hasMore\":%s}",total,offset,limit,(size_t)(offset+emitted)<total?"true":"false");return 0;
}
