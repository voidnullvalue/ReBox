#ifndef REBOX_AUTH_H
#define REBOX_AUTH_H
#include "core.h"
typedef struct {char secret[65],token[65],code[9],path[REBOX_PATH_MAX];double expires;unsigned attempts;} RbAuth;
int rb_random(char *,size_t);
int rb_auth_init(RbAuth *,const char *);
int rb_authorized(const RbAuth *,const char *);
int rb_auth_open(RbAuth *,struct sb *,double);
int rb_auth_pair(RbAuth *,const char *,struct sb *,double);
int rb_auth_revoke(RbAuth *);
#endif
