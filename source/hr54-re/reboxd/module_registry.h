#ifndef REBOX_REGISTRY_H
#define REBOX_REGISTRY_H
#include "core.h"
int rb_registry_load(ReboxRegistry *,const char *);
ReboxModule *rb_registry_find(ReboxRegistry *,const char *);
int rb_state_save(const ReboxRegistry *,const ReboxModule *);
int rb_registry_json(const ReboxRegistry *,struct sb *);
void rb_module_json(const ReboxModule *,struct sb *);
#endif
