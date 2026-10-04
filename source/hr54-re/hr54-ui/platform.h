#ifndef HR54_UI_PLATFORM_H
#define HR54_UI_PLATFORM_H
#ifdef HR54_RECEIVER
#include "receiver-include/libc.h"
#define EINPROGRESS 150
extern int flock(int,int);
#define SO_ERROR 0x1007
extern int getsockopt(int,int,int,void *,unsigned *);
/* uClibc LinuxThreads on this receiver uses an unsigned-long thread ID. */
typedef unsigned long pthread_t;
extern int pthread_create(pthread_t *,const void *,void *(*)(void *),void *);
extern int pthread_join(pthread_t,void **);
extern int pipe(int *);
#else
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <pthread.h>
#include <arpa/inet.h>
#endif
static inline uint64_t ui_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000; }
static inline void ui_copy(char *d,size_t n,const char *s) { if(n) snprintf(d,n,"%s",s?s:""); }
#endif
