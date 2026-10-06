#include "json.h"
static void ws(Json *j,size_t *p) { while(*p<j->length && (j->text[*p]==' '||j->text[*p]=='\r'||j->text[*p]=='\n'||j->text[*p]=='\t')) ++*p; }
static int hex(char c) { if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return -1; }
static int value(Json *j,size_t *p,int depth) {
    ws(j,p); if(depth>32||*p>=j->length||j->count>=JSON_MAX_TOKENS)return -1;
    int i=j->count++; JsonToken *t=&j->t[i]; t->start=(int)*p; char c=j->text[(*p)++]; t->type=c;
    if(c=='{'||c=='[') {
        ws(j,p); char end=c=='{'?'}':']';
        if(*p<j->length&&j->text[*p]==end)++*p;
        else for(;;) {
            if(c=='{') { ws(j,p); if(*p>=j->length||j->text[*p]!='"'||value(j,p,depth+1)<0)return -1; ws(j,p); if(*p>=j->length||j->text[(*p)++]!=':')return -1; }
            if(value(j,p,depth+1)<0)return -1; t->count++; ws(j,p);
            if(*p>=j->length)return -1; char sep=j->text[(*p)++]; if(sep==end)break; if(sep!=',')return -1;
        }
    } else if(c=='"') {
        int closed=0;
        while(*p<j->length) {
            unsigned char x=j->text[(*p)++]; if(x=='"'){closed=1;break;} if(x<32)return -1;
            if(x=='\\') { if(*p>=j->length)return -1; x=j->text[(*p)++]; if(x=='u'){for(int k=0;k<4;k++)if(*p>=j->length||hex(j->text[(*p)++])<0)return -1;} else if(!strchr("\"\\/bfnrt",x))return -1; }
        }
        if(!closed)return -1;
    } else if(c=='t'||c=='f'||c=='n') {
        const char *s=c=='t'?"true":c=='f'?"false":"null"; size_t n=strlen(s);
        if((size_t)t->start+n>j->length||strncmp(j->text+t->start,s,n))return -1; *p=(size_t)t->start+n;
    } else {
        t->type='0'; *p=t->start; if(j->text[*p]=='-')++*p;
        if(*p>=j->length)return -1;
        if(j->text[*p]=='0')++*p; else { if(j->text[*p]<'1'||j->text[*p]>'9')return -1; while(*p<j->length&&j->text[*p]>='0'&&j->text[*p]<='9')++*p; }
        if(*p<j->length&&j->text[*p]=='.'){++*p;size_t s=*p;while(*p<j->length&&j->text[*p]>='0'&&j->text[*p]<='9')++*p;if(s==*p)return -1;}
        if(*p<j->length&&(j->text[*p]=='e'||j->text[*p]=='E')){++*p;if(*p<j->length&&(j->text[*p]=='+'||j->text[*p]=='-'))++*p;size_t s=*p;while(*p<j->length&&j->text[*p]>='0'&&j->text[*p]<='9')++*p;if(s==*p)return -1;}
    }
    t->end=(int)*p;t->next=j->count;return i;
}
int json_open(Json *j,const char *s,size_t n) { memset(j,0,sizeof(*j));j->text=s;j->length=n;j->t=calloc(JSON_MAX_TOKENS,sizeof(*j->t));if(!j->t)return -1;size_t p=0;if(value(j,&p,0)<0){json_close(j);return -1;}ws(j,&p);if(p!=n){json_close(j);return -1;}return 0; }
void json_close(Json *j){free(j->t);j->t=NULL;}
int json_field(const Json *j,int i,const char *key) { if(i<0||i>=j->count||j->t[i].type!='{')return -1;for(int k=i+1;k<j->t[i].next;){JsonToken *t=&j->t[k];int v=k+1;if(t->end-t->start==(int)strlen(key)+2&&!strncmp(j->text+t->start+1,key,strlen(key)))return v;k=j->t[v].next;}return -1; }
int json_nth(const Json *j,int i,int n){if(i<0||i>=j->count||j->t[i].type!='['||n<0)return -1;int k=i+1;while(n--&&k<j->t[i].next)k=j->t[k].next;return k<j->t[i].next?k:-1;}
static void emit(char *d,size_t cap,size_t *n,unsigned cp) { unsigned char b[4];int len=1;if(cp<128)b[0]=cp;else if(cp<2048){b[0]=192|(cp>>6);b[1]=128|(cp&63);len=2;}else if(cp<65536){b[0]=224|(cp>>12);b[1]=128|((cp>>6)&63);b[2]=128|(cp&63);len=3;}else{b[0]=240|(cp>>18);b[1]=128|((cp>>12)&63);b[2]=128|((cp>>6)&63);b[3]=128|(cp&63);len=4;}if(*n+(size_t)len<cap)memcpy(d+*n,b,len);*n+=len; }
int json_string(const Json *j,int i,char *d,size_t cap) {
    if(!cap)return -1;d[0]=0;if(i<0||i>=j->count||j->t[i].type!='"')return -1;size_t n=0;
    for(int p=j->t[i].start+1;p<j->t[i].end-1;p++){unsigned char c=j->text[p];if(c=='\\'){c=j->text[++p];if(c=='u'){unsigned cp=0;for(int k=0;k<4;k++)cp=cp*16+hex(j->text[++p]);if(cp>=0xd800&&cp<=0xdbff&&p+6<j->t[i].end&&j->text[p+1]=='\\'&&j->text[p+2]=='u'){unsigned lo=0;p+=2;for(int k=0;k<4;k++)lo=lo*16+hex(j->text[++p]);if(lo<0xdc00||lo>0xdfff)return -1;cp=0x10000+((cp-0xd800)<<10)+lo-0xdc00;}if(!cp||(cp>=0xd800&&cp<=0xdfff))return -1;emit(d,cap,&n,cp);continue;} if(c=='n'||c=='r'||c=='t')c=' ';else if(c=='b'||c=='f')continue;}if(n+1<cap)d[n]=c;n++;}
    d[n<cap?n:cap-1]=0;return n<cap?0:-1;
}
int json_bool(const Json *j,int i){return i>=0&&i<j->count&&j->t[i].type=='t';}
long json_number(const Json *j,int i){if(i<0||i>=j->count||j->t[i].type!='0')return 0;char b[40];int n=j->t[i].end-j->t[i].start;if(n>38)return 0;memcpy(b,j->text+j->t[i].start,n);b[n]=0;long v=strtol(b,NULL,10);return v;}
int json_quote(char *d,size_t cap,const char *s){size_t n=0;if(cap<3)return -1;d[n++]='"';for(;*s;s++){unsigned char c=*s;if(c<32)return -1;if(n+3>=cap)return -1;if(c=='"'||c=='\\')d[n++]='\\';d[n++]=c;}d[n++]='"';d[n]=0;return 0;}
