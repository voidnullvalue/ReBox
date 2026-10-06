#ifndef REBOX_HTTP_H
#define REBOX_HTTP_H
#include "core.h"
#define RB_BODY_MAX 16384
#define RB_RESPONSE_MAX (2*1024*1024)
typedef struct {char method[8],path[2048],authorization[80],body[RB_BODY_MAX+1];size_t length;} RbRequest;
int rb_http_read(int,RbRequest *);
void rb_http_reply(int,int,const char *,const void *,size_t);
void rb_http_json(int,int,const char *);
void rb_http_error(int,int,const char *);
int rb_listen_tcp(int);
int rb_listen_unix(const char *);
#endif
