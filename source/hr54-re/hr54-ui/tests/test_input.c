#include "input/dispatcher.h"
#include <assert.h>
static int presses,repeats,releases;
static void sink(void *ctx,KeyEvent ev){(void)ctx;assert(ev.key==KEY_RIGHT);if(ev.pressed){presses++;repeats+=ev.repeat;}else releases++;}
int main(int argc,char **argv){(void)argv;Input in={.fd=-1};assert(!input_open(&in,0));in.wanted_active=0;const int modes[]={1,2,1,0,1};int step=0;uint64_t end=ui_now()+4000;while(ui_now()<end){int rc=input_pump(&in,ui_now(),sink,NULL);if(argc>1){if(rc<0){assert(in.fd<0);return 0;}}else assert(rc>=0);if(in.ready){if(step<5){assert(!input_mode(&in,modes[step++]));}else if(releases){assert(presses==2&&repeats==1&&releases==1);input_close(&in);return 0;}}struct timespec d={0,1000000};nanosleep(&d,NULL);}return 1;}
