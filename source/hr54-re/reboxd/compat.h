#ifndef REBOX_COMPAT_H
#define REBOX_COMPAT_H
#include "module_registry.h"
#include "http.h"
/* Returns one only when this isolated legacy adapter handled the route. */
int rb_compat(int, const RbRequest *, ReboxRegistry *);
#endif
