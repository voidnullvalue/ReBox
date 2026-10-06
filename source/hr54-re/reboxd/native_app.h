#ifndef REBOX_NATIVE_APP_H
#define REBOX_NATIVE_APP_H
#include "module_registry.h"
int rb_native_init(const ReboxRegistry *);
int rb_native_busy(const char *);
void rb_native_system_json(struct sb *);
void rb_native_tick(const ReboxRegistry *);
int rb_native_operation(ReboxModule *,const char *,const char *,struct sb *);
#endif
