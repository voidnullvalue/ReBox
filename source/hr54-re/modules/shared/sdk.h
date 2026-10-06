#ifndef REBOX_SDK_H
#define REBOX_SDK_H
#include "../../reboxd/http.h"
typedef void (*RbModuleHandler)(int,const RbRequest *);
int rb_module_serve(RbModuleHandler);
#endif
