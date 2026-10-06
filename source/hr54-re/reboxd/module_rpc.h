#ifndef REBOX_RPC_H
#define REBOX_RPC_H
#include "http.h"
typedef struct {int status;char type[128];struct sb body;} RbReply;
int rb_rpc_open(const ReboxModule *,const char *,const char *,const char *,int);
int rb_rpc(const ReboxModule *,const char *,const char *,const char *,RbReply *,int);
void rb_reply_free(RbReply *);
int rb_rpc_health(const ReboxModule *);
#endif
