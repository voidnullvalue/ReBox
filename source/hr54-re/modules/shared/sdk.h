#ifndef REBOX_SDK_H
#define REBOX_SDK_H
#include "../../reboxd/http.h"
typedef void (*RbModuleHandler)(int,const RbRequest *);
int rb_module_serve(RbModuleHandler);
/* Copy package-owned defaults into private data without replacing user files. */
int rb_module_seed(const char *,const char *);
int rb_module_copy_defaults(const char *,const char *);
/* Log only a URL's public scheme/authority; never paths, queries or userinfo. */
void rb_media_origin(const char *,const char *);
#endif
