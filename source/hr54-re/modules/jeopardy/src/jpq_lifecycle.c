#include "jpq_lifecycle.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void nap(double seconds) {
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)(time_t)seconds) * 1e9);
    nanosleep(&ts, NULL);
}

static void seterr(char *err, size_t cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg);
}

/* A PID is ours only if it is alive AND its executable is the expected engine;
 * this prevents ever signalling a recycled PID. */
int jpq_lifecycle_pid(const char *engine, const char *pid_path) {
    FILE *f = fopen(pid_path, "r");
    if (!f) return -1;
    int pid = -1;
    if (fscanf(f, "%d", &pid) != 1) pid = -1;
    fclose(f);
    if (pid <= 1 || kill(pid, 0)) return -1;
    char proc[64], actual[1024];
    snprintf(proc, sizeof proc, "/proc/%d/exe", pid);
    ssize_t n = readlink(proc, actual, sizeof actual - 1);
    if (n < 0) return -1;
    actual[n] = 0;
    return !strcmp(actual, engine) ? pid : -1;
}

/* The lock is held for the whole engine lifetime; its release is the proof the
 * surface is gone, not the disappearance of the PID. */
int jpq_lifecycle_busy(const char *lock_path) {
    int fd = open(lock_path, O_RDONLY);
    if (fd < 0) return errno != ENOENT;
    int busy = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);
    return busy;
}

int jpq_lifecycle_stop(const char *engine, const char *pid_path, const char *lock_path, int *was_running) {
    if (was_running) *was_running = 0;
    int pid = jpq_lifecycle_pid(engine, pid_path);
    if (pid < 0) return 0;
    if (was_running) *was_running = 1;
    kill(pid, SIGTERM);
    for (int i = 0; i < 20 && jpq_lifecycle_busy(lock_path); i++) nap(0.25);
    if (jpq_lifecycle_busy(lock_path)) return -1; /* surface recovery needs review */
    unlink(pid_path);
    return 0;
}

int jpq_lifecycle_start(const char *engine, const char *pid_path, const char *lock_path,
                        const char *depth_arg, int timeout_ms, int *out_pid, char *err, size_t errcap) {
    if (jpq_lifecycle_busy(lock_path)) { seterr(err, errcap, "surface lock retained"); return -1; }
    if (access(engine, X_OK)) { seterr(err, errcap, "native executable unavailable"); return -1; }
    int lf = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (lf < 0 || flock(lf, LOCK_EX | LOCK_NB)) { if (lf >= 0) close(lf); seterr(err, errcap, "lifecycle busy"); return -1; }
    int ready[2];
    if (pipe(ready)) { close(lf); seterr(err, errcap, "launch pipe failed"); return -1; }
    pid_t reaper = fork();
    if (reaper < 0) { close(ready[0]); close(ready[1]); close(lf); seterr(err, errcap, "launch failed"); return -1; }
    if (!reaper) {
        close(ready[0]);
        setsid();
        signal(SIGCHLD, SIG_DFL);
        alarm(0);
        for (int f = 3; f < 1024; f++) if (f != lf && f != ready[1]) close(f);
        int execution[2], initialized[2];
        if (pipe(execution) || pipe(initialized)) _exit(1);
        fcntl(execution[1], F_SETFD, FD_CLOEXEC);
        pid_t engine_pid = fork();
        if (!engine_pid) {
            close(execution[0]); close(initialized[0]); close(lf); close(ready[1]);
            char fdarg[16];
            snprintf(fdarg, sizeof fdarg, "%d", initialized[1]);
            execl(engine, "hr54-jeopardy-native", "--depth", depth_arg, "--ready-fd", fdarg, (char *)NULL);
            int error = errno;
            write(execution[1], &error, sizeof error);
            _exit(127);
        }
        close(execution[1]);
        close(initialized[1]);
        int error = 0;
        ssize_t n = read(execution[0], &error, sizeof error);
        close(execution[0]);
        int recorded = 0;
        if (engine_pid > 1 && n == 0) {
            FILE *p = fopen(pid_path, "w");
            if (p) { fprintf(p, "%d\n", (int)engine_pid); recorded = fclose(p) == 0; }
        }
        struct pollfd init = {initialized[0], POLLIN, 0};
        unsigned char ok = 0;
        if (recorded && poll(&init, 1, timeout_ms) > 0 && read(initialized[0], &ok, 1) == 1 && ok == 1)
            write(ready[1], &engine_pid, sizeof engine_pid);
        else if (engine_pid > 1)
            kill(engine_pid, SIGTERM);
        close(initialized[0]);
        if (engine_pid > 1) { int status; while (waitpid(engine_pid, &status, 0) < 0 && errno == EINTR) {} }
        unlink(pid_path);
        close(lf);
        close(ready[1]);
        _exit(0);
    }
    close(ready[1]);
    close(lf);
    struct pollfd p = {ready[0], POLLIN, 0};
    pid_t engine_pid = 0;
    int rc = poll(&p, 1, timeout_ms + 500);
    ssize_t n = rc > 0 ? read(ready[0], &engine_pid, sizeof engine_pid) : -1;
    close(ready[0]);
    if (n != sizeof engine_pid || engine_pid <= 1) { seterr(err, errcap, "native surface failed to initialize"); return -1; }
    if (out_pid) *out_pid = (int)engine_pid;
    return 0;
}
