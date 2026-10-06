#ifndef REBOX_PLAYBACK_H
#define REBOX_PLAYBACK_H
#include "module_registry.h"
int rb_play(ReboxRegistry *,ReboxModule *,const char *,struct sb *);
int rb_transport(ReboxRegistry *,const char *,const char *,struct sb *);
void rb_playback_json(struct sb *);
int rb_playback_busy(const char *);
#endif
