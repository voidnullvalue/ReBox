#ifndef REBOX_PROCESS_H
#define REBOX_PROCESS_H
#include "module_registry.h"
int rb_process_init(const char *);
int rb_process_start(ReboxRegistry *,ReboxModule *,int);
int rb_process_stop(ReboxModule *);
void rb_process_tick(ReboxRegistry *);
void rb_process_shutdown(ReboxRegistry *);
#endif
