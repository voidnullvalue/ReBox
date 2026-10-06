#ifndef REBOX_MANAGER_H
#define REBOX_MANAGER_H
#include "module_registry.h"
int rb_manager_install(ReboxRegistry *,const char *,const char *,const char *,struct sb *);
int rb_manager_mutate(ReboxRegistry *,const char *,const char *,struct sb *);
int rb_manager_recover(ReboxRegistry *);
#endif
