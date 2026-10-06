#ifndef REBOX_PACKAGE_H
#define REBOX_PACKAGE_H
#include "core.h"
#define RB_PACKAGE_MAX (64u*1024u*1024u)
#define RB_EXTRACT_MAX (128u*1024u*1024u)
int rb_package_extract(const char *,const char *);
int rb_package_sha(const char *,char [65]);
int rb_download(const char *,const char *);
int rb_remove_tree(const char *);
#endif
