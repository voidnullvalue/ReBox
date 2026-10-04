#ifndef HR54_UI_API_INTERNAL_H
#define HR54_UI_API_INTERNAL_H
#include "services.h"
static inline int get(ApiClient *a,int slot,ApiKind k,const char *path,unsigned timeout){return api_send(a,slot,k,"GET",path,NULL,timeout);}
static inline int post(ApiClient *a,int slot,ApiKind k,const char *path,const char *body,unsigned timeout){return api_send(a,slot,k,"POST",path,body,timeout);}
#endif
