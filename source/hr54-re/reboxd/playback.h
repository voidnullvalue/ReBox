#ifndef REBOX_PLAYBACK_H
#define REBOX_PLAYBACK_H
#include "module_registry.h"
void rb_playback_port(int);
void rb_playback_tick(void);
void rb_stream_proxy(int,const char *);
int rb_play(ReboxRegistry *,ReboxModule *,const char *,struct sb *);
int rb_transport(ReboxRegistry *,const char *,const char *,struct sb *);
void rb_playback_json(struct sb *);
int rb_playback_busy(const char *);
#endif
