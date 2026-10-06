#ifndef REBOX_CORE_H
#define REBOX_CORE_H
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "json.h"
#define REBOX_API 1
#define REBOX_MAX_MODULES 32
#define REBOX_MODULE_ID_MAX 64
#define REBOX_PATH_MAX 1024
#define REBOX_MANIFEST_MAX 16384
#define REBOX_ROOT "/var/hr54-persist/rebox"
typedef enum { REBOX_MODULE_MEDIA, REBOX_MODULE_NATIVE_APP } ReboxModuleKind;
typedef struct {
    char id[64], name[128], version[64], description[256], home_description[256];
    char entrypoint[256], icon[256], error[160], package[128], sha256[65];
    ReboxModuleKind kind;
    int installed, enabled, core, compatible, healthy, default_enabled, order, home;
    int browse, search, settings, auth, playback, native_app, release_surface, release_input;
    pid_t pid;
    char executable[REBOX_PATH_MAX], socket[108];
    double started;
    int restart_count;
} ReboxModule;
typedef struct {
    char root[512];
    ReboxModule modules[REBOX_MAX_MODULES];
    size_t count;
} ReboxRegistry;
int rb_id(const char *);
int rb_relative(const char *);
int rb_mkdir(const char *);
char *rb_read(const char *, size_t, size_t *);
int rb_atomic(const char *, const void *, size_t);
int rb_path(char *, size_t, const char *, const char *, const char *);
void rb_log(const char *, const char *);
#endif
