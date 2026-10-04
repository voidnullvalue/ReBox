#include "libc.h"
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) ((x) ? (void)0 : (fprintf(stderr,"assertion failed: %s (%s:%d)\n",#x,__FILE__,__LINE__),abort()))
#endif
