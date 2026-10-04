/* Small uClibc-linked adapter to the receiver's installed curl/wolfSSL.
 * stdout: effective URL newline, content type newline, blank line, body.
 * No shell expansion; caller passes trusted playlist headers as argv. */
#include <stddef.h>
typedef long ssize_t;
extern ssize_t write(int,const void *,size_t);
extern size_t strlen(const char *);
extern char *strchr(const char *,int);
extern char *strpbrk(const char *,const char *);
extern int strcmp(const char *,const char *);
extern int strncmp(const char *,const char *,size_t);
extern long atol(const char *);
extern int puts(const char *);
extern int fprintf(void *,const char *,...);
extern void *stderr;
typedef void CURL;
extern CURL *curl_easy_init(void);
extern int curl_easy_setopt(CURL *, int, ...);
extern int curl_easy_perform(CURL *);
extern int curl_easy_getinfo(CURL *, int, ...);
extern void curl_easy_cleanup(CURL *);
extern const char *curl_easy_strerror(int);
extern char *curl_version(void);
static CURL *handle;
static int sent;
static int put(const void *p,size_t n){const char *b=p;while(n){ssize_t k=write(1,b,n);if(k<=0)return -1;b+=k;n-=k;}return 0;}
static size_t body(char *p,size_t s,size_t n,void *unused){
 (void)unused; size_t len=s*n;
 if(!sent){char *url=NULL,*ct=NULL;long code=0;curl_easy_getinfo(handle,0x100001,&url);curl_easy_getinfo(handle,0x100012,&ct);curl_easy_getinfo(handle,0x200002,&code);
 if(code<200||code>=300)return 0;
 if(!url||strlen(url)>8192||strchr(url,'\n')||(ct&&(strlen(ct)>256||strpbrk(ct,"\r\n"))))return 0;
 if(put(url,strlen(url))||put("\n",1)||put(ct?ct:"",ct?strlen(ct):0)||put("\n\n",2))return 0;sent=1;}
 return put(p,len)?0:len;
}
int main(int argc,char **argv){
 if(argc==2&&!strcmp(argv[1],"--version")){puts(curl_version());return 0;}
 if(argc!=6)return 2;
 handle=curl_easy_init();if(!handle)return 3;
 #define OPT(k,v) do{if(curl_easy_setopt(handle,k,v))return 4;}while(0)
 OPT(10002,argv[1]);OPT(20011,body);OPT(10001,(void *)NULL);
 OPT(10065,argv[2]);OPT(64,1L);OPT(81,2L);
 OPT(52,1L);OPT(68,5L);OPT(181,3L);OPT(182,!strncmp(argv[1],"https://",8)?2L:3L);
 OPT(78,12L);OPT(13,atol(argv[5]));OPT(45,1L);OPT(19,1L);OPT(20,15L);OPT(99,1L);
 OPT(10018,*argv[3]?argv[3]:"HR54-IPTV/1");if(*argv[4])OPT(10016,argv[4]);
 int rc=curl_easy_perform(handle);if(rc)fprintf(stderr,"IPTV fetch: %s\n",curl_easy_strerror(rc));curl_easy_cleanup(handle);return rc?1:0;
}
#ifdef HR54_UCLIBC_START
extern void __uClibc_main(int (*)(int,char **),int,char **,void *,void *,void *,void *);
void iptv_entry(long *stack){__uClibc_main(main,(int)stack[0],(char **)(stack+1),NULL,NULL,NULL,stack);}
__asm__(".text\n.globl __start\n.ent __start\n__start:\nlui $28,%hi(_gp)\naddiu $28,$28,%lo(_gp)\nmove $4,$29\naddiu $29,$29,-32\nla $25,iptv_entry\njalr $25\nnop\nb .\nnop\n.end __start\n");
#endif
