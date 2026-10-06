/* Receiver curl ABI follows the existing hr54-iptv-fetch adapter. */
#include <stddef.h>
extern long write(int,const void *,size_t);
extern size_t strlen(const char *);
extern int strncmp(const char *,const char *,size_t);
extern void *curl_easy_init(void);
extern int curl_easy_setopt(void *,int,...);
extern int curl_easy_perform(void *);
extern int curl_easy_getinfo(void *,int,...);
extern void curl_easy_cleanup(void *);
static void *curl;
static size_t total;
static size_t receive(char *p,size_t s,size_t n,void *unused){(void)unused;size_t len=s*n;long status=0;curl_easy_getinfo(curl,0x200002,&status);if(status>=300&&status<400)return len;if(status<200||status>=300||len>64u*1024*1024-total)return 0;total+=len;size_t remaining=len;while(remaining){long k=write(1,p,remaining);if(k<=0)return 0;p+=k;remaining-=k;}return len;}
int main(int argc,char **argv){if(argc!=2)return 2;curl=curl_easy_init();if(!curl)return 1;
#define OPT(k,v) do{if(curl_easy_setopt(curl,k,v))return 1;}while(0)
    OPT(10002,argv[1]);OPT(20011,receive);OPT(64,1L);OPT(81,2L);OPT(52,1L);OPT(68,5L);OPT(181,3L);OPT(182,!strncmp(argv[1],"https://",8)?2L:3L);OPT(78,12L);OPT(13,180L);OPT(45,1L);OPT(19,1024L);OPT(20,15L);OPT(99,1L);
#ifdef REBOX_RECEIVER
    OPT(10065,"/var/hr54-persist/rebox/ca-certificates.crt");
#endif
    int rc=curl_easy_perform(curl);curl_easy_cleanup(curl);return rc||!total?1:0;}
#ifdef HR54_UCLIBC_START
extern void __uClibc_main(int (*)(int,char **),int,char **,void *,void *,void *,void *);
void fetch_entry(long *s){__uClibc_main(main,(int)s[0],(char **)(s+1),NULL,NULL,NULL,s);}
__asm__(".text\n.globl __start\n.ent __start\n__start:\nlui $28,%hi(_gp)\naddiu $28,$28,%lo(_gp)\nmove $4,$29\naddiu $29,$29,-32\nla $25,fetch_entry\njalr $25\nnop\nb .\nnop\n.end __start\n");
#endif
