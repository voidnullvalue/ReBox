#ifndef HR54_LIBC_DECLARATIONS_H
#define HR54_LIBC_DECLARATIONS_H
/* Minimal receiver o32 uClibc declarations for the existing Doom C engine.
 * FILE stays opaque. All stateful ABI structs used here have 32-bit fields.
 * No third-party libc implementation or musl header is included. */
#include "../receiver_runtime.h"
#include <stdarg.h>
extern FILE *stdin,*stdout;
extern int printf(const char *,...);
extern int vfprintf(FILE *,const char *,va_list);
extern int vsnprintf(char *,size_t,const char *,va_list);
extern int sscanf(const char *,const char *,...);
extern int fscanf(FILE *,const char *,...);
extern int sprintf(char *,const char *,...);
extern int puts(const char *);
extern int putchar(int);
extern FILE *fopen(const char *,const char *);
extern int fclose(FILE *);
extern int fflush(FILE *);
extern int fseek(FILE *,long,int);
extern long ftell(FILE *);
extern size_t fread(void *,size_t,size_t,FILE *);
extern size_t fwrite(const void *,size_t,size_t,FILE *);
extern int fgetc(FILE *);
extern int fputc(int,FILE *);
extern char *fgets(char *,int,FILE *);
extern int feof(FILE *);
extern int ferror(FILE *);
extern int fileno(FILE *);
extern int rename(const char *,const char *);
extern int remove(const char *);
extern int setvbuf(FILE *,char *,int,size_t);
extern void perror(const char *);
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IONBF 2
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
extern int atexit(void (*)(void));
extern void abort(void);
extern void *realloc(void *,size_t);
extern int atoi(const char *);
extern unsigned long strtoul(const char *,char **,int);
extern double atof(const char *);
extern int abs(int);
extern char *getenv(const char *);
extern void qsort(void *,size_t,size_t,int (*)(const void *,const void *));
extern size_t strlen(const char *);
extern int strncmp(const char *,const char *,size_t);
extern int strcasecmp(const char *,const char *);
extern int strncasecmp(const char *,const char *,size_t);
extern char *strdup(const char *);
extern char *strcpy(char *,const char *);
extern char *strncpy(char *,const char *,size_t);
extern char *strcat(char *,const char *);
extern char *strchr(const char *,int);
extern char *strrchr(const char *,int);
extern char *strstr(const char *,const char *);
extern int memcmp(const void *,const void *,size_t);
extern void *memmove(void *,const void *,size_t);
extern char *strerror(int);
extern int toupper(int),tolower(int),isspace(int),isdigit(int),isprint(int),isalpha(int);
extern int *__errno_location(void);
#define errno (*__errno_location())
#define EINTR 4
#define EIO 5
#define EAGAIN 11
#define EISDIR 21
#define ENOENT 2
#define SIGPIPE 13
#define SIG_IGN ((void (*)(int))1)
typedef int ssize_t;
typedef unsigned mode_t;
typedef long time_t;
extern int open(const char *,int,...);
extern int close(int);
extern ssize_t read(int,void *,size_t);
extern ssize_t write(int,const void *,size_t);
extern int unlink(const char *);
extern int mkdir(const char *,mode_t);
extern int isatty(int);
extern char *getcwd(char *,size_t);
extern int fcntl(int,int,...);
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0400
#define O_TRUNC 01000
#define O_NONBLOCK 0200
#define F_GETFL 3
#define F_SETFL 4
#define F_SETFD 2
#define FD_CLOEXEC 1
struct pollfd { int fd; short events,revents; };
#define POLLIN 1
#define POLLOUT 4
extern int poll(struct pollfd *,unsigned long,int);
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
struct sockaddr_in { unsigned short sin_family,sin_port; struct { unsigned s_addr; } sin_addr; unsigned char sin_zero[8]; };
struct timeval { long tv_sec,tv_usec; };
#define AF_INET 2
#define SOCK_STREAM 2
#define SOL_SOCKET 65535
#define SO_RCVTIMEO 0x1006
#define SO_SNDTIMEO 0x1005
#define SO_REUSEADDR 4
#define MSG_NOSIGNAL 0x4000
extern int socket(int,int,int);
extern int connect(int,const struct sockaddr *,unsigned);
extern int bind(int,const struct sockaddr *,unsigned);
extern int listen(int,int);
extern int accept(int,struct sockaddr *,unsigned *);
extern int setsockopt(int,int,int,const void *,unsigned);
extern ssize_t send(int,const void *,size_t,int);
extern ssize_t recv(int,void *,size_t,int);
extern int system(const char *);
_Static_assert(sizeof(struct sockaddr_in)==16 && sizeof(struct timeval)==8,
               "receiver socket/time declarations require o32");
#endif
