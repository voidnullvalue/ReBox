#ifndef HR54_UI_DISPATCHER_H
#define HR54_UI_DISPATCHER_H
#include "keys.h"
typedef struct { int fd,phase,active,wanted_active,registered_mode,manual,ready,failed,trace; unsigned port; uint32_t owner; unsigned char rx[512],tx[320];size_t used,sent,size;uint64_t deadline;unsigned char held[KEY_COUNT]; } Input;
typedef void (*KeySink)(void *,KeyEvent);
int input_open(Input *,int);
int input_open_broker(Input *,int);
int input_mode(Input *,int);
int input_pump(Input *,uint64_t,KeySink,void *);
void input_close(Input *);
#endif
