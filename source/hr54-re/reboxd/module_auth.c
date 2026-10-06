#include "module_auth.h"
int rb_random(char *out,size_t chars){unsigned char bytes[32];if(!chars||chars>64||chars%2)return -1;int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);if(fd<0)return -1;size_t got=0;while(got<chars/2){ssize_t n=read(fd,bytes+got,chars/2-got);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);return -1;}got+=n;}close(fd);for(size_t i=0;i<chars;i++)out[i]="0123456789abcdef"[(bytes[i/2]>>(i%2?0:4))&15];out[chars]=0;return 0;}
static int equal(const char *a,const char *b,size_t n){unsigned diff=0;for(size_t i=0;i<n;i++)diff|=(unsigned char)a[i]^(unsigned char)b[i];return diff==0;}
static int secret_read(const char *path,char out[65]){struct stat st;if(lstat(path,&st))return -1;if(!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&077))return -1;size_t n;char *s=rb_read(path,64,&n);if(!s)return -1;int good=n==64&&strspn(s,"0123456789abcdef")==64;if(good)memcpy(out,s,65);free(s);return good?0:-1;}
int rb_auth_init(RbAuth *a,const char *root){memset(a,0,sizeof *a);char path[REBOX_PATH_MAX];rb_path(path,sizeof path,root,"module-state/management.secret",NULL);
    if(secret_read(path,a->secret)){if(errno!=ENOENT)return fail("management secret unreadable");if(rb_random(a->secret,64)||rb_atomic(path,a->secret,64))return -1;}
    rb_path(a->path,sizeof a->path,root,"module-state/management.token",NULL);if(secret_read(a->path,a->token)&&errno!=ENOENT)return fail("management token unreadable");return 0;
}
int rb_authorized(const RbAuth *a,const char *header){if(strncmp(header,"Bearer ",7)||strlen(header)!=71)return 0;return equal(a->secret,header+7,64)||(*a->token&&equal(a->token,header+7,64));}
int rb_auth_open(RbAuth *a,struct sb *out,double now){if(rb_random(a->code,8))return 500;a->expires=now+120;a->attempts=0;sb_fmt(out,"{\"ok\":true,\"code\":\"%s\",\"expiresIn\":120}",a->code);return 200;}
int rb_auth_pair(RbAuth *a,const char *body,struct sb *out,double now){struct jval *v=json_parse(body,strlen(body));const char *code=jstr(jget(v,"code"));int ok=*a->code&&now<a->expires&&a->attempts++<5&&code&&strlen(code)==8&&equal(a->code,code,8);jfree(v);if(!ok){if(now>=a->expires||a->attempts>=5)a->code[0]=0;return 403;}
    a->code[0]=0;char token[65];if(rb_random(token,64)||rb_atomic(a->path,token,64))return 500;memcpy(a->token,token,65);sb_fmt(out,"{\"ok\":true,\"token\":\"%s\"}",token);return 200;
}
int rb_auth_revoke(RbAuth *a){if(unlink(a->path)&&errno!=ENOENT)return 500;memset(a->token,0,sizeof a->token);a->code[0]=0;return 200;}
