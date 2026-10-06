#ifndef REBOX_MANIFEST_H
#define REBOX_MANIFEST_H
#include "core.h"
int rb_manifest(const char *,size_t,ReboxModule *);
int rb_manifest_file(const char *,ReboxModule *);
int rb_module_files(const char *,ReboxModule *);
#endif
