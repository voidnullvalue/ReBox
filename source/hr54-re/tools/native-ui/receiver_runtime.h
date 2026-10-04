#ifndef HR54_RECEIVER_RUNTIME_H
#define HR54_RECEIVER_RUNTIME_H
/* Narrow o32 uClibc 0.9.32.1 declarations. No musl FILE/time/stat layouts.
 * The build uses compiler builtin integer headers and receiver shared libs. */
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
typedef struct Hr54File FILE;
extern FILE *stderr;
extern int fprintf(FILE *, const char *, ...);
extern int snprintf(char *, size_t, const char *, ...);
extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern void free(void *);
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern int strcmp(const char *, const char *);
extern long strtol(const char *, char **, int);
extern void exit(int);
extern int getpid(void);
extern unsigned alarm(unsigned);
typedef int sig_atomic_t;
extern void (*signal(int, void (*)(int)))(int);
#define SIGINT 2
#define SIGTERM 15
#define SIGALRM 14
struct timespec { long tv_sec, tv_nsec; };
_Static_assert(sizeof(void *)==4 && sizeof(long)==4 && sizeof(struct timespec)==8,
               "receiver runtime declarations require MIPS o32");
extern int clock_gettime(int, struct timespec *);
extern int nanosleep(const struct timespec *, struct timespec *);
#define CLOCK_MONOTONIC 1
#endif
