#include "module_auth.h"
#include <assert.h>
int main(int argc,char **argv){assert(argc==2);RbAuth a;assert(!rb_auth_init(&a,argv[1]));struct sb b={0};assert(rb_auth_open(&a,&b,100)==200);free(b.p);memset(&b,0,sizeof b);char body[64];snprintf(body,sizeof body,"{\"code\":\"%s\"}",a.code);assert(rb_auth_pair(&a,body,&b,221)==403);assert(!*a.code);assert(rb_auth_open(&a,&b,300)==200);free(b.p);memset(&b,0,sizeof b);for(int i=0;i<5;i++)assert(rb_auth_pair(&a,"{\"code\":\"wrong\"}",&b,301)==403);assert(!*a.code);puts("PASS pairing expiry and attempt limit");return 0;}
