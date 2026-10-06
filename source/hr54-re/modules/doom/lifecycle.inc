/* Returns 0 on success, -1 if the path would not fit. */
static int doom_path(char *out, size_t n, const char *rel) {
#ifdef REBOX_DOOM_MODULE
    const char *root=!strcmp(rel,"bin/hr54-doom-native")&&*package_engine?package_engine:NULL;
    int k=root?snprintf(out,n,"%s",root):snprintf(out,n,"%s/doom/%s",persist_root,rel);
#else
    int k = snprintf(out, n, "%s/doom/%s", persist_root, rel);
#endif
    return (k < 0 || (size_t)k >= n) ? -1 : 0;
}

static int doom_armed(void) {
    char path[512];
    struct stat st;
    if (doom_path(path, sizeof path, "DISABLED") == 0 && stat(path, &st) == 0)
        return 0;
#ifndef REBOX_DOOM_MODULE
    if (doom_path(path, sizeof path, "ENABLED") || stat(path, &st)) return 0;
#endif
    return 1;
}

static int doom_running_pid(void) {
    char path[512];
    if (doom_path(path, sizeof path, "doom.pid")) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int pid = -1;
    if (fscanf(f, "%d", &pid) != 1) pid = -1;
    fclose(f);
    if (pid <= 1) return -1;
    if (kill(pid, 0) && errno == ESRCH) return -1;
    if (native_frontend) {
        char proc[64], exe[512], expected[512];
        snprintf(proc, sizeof proc, "/proc/%d/exe", pid);
        ssize_t n = readlink(proc, exe, sizeof exe - 1);
        if (n < 0) return -1;
        exe[n] = 0;
        doom_path(expected, sizeof expected, "bin/hr54-doom-native");
        if (strcmp(exe, expected)) return -1; /* Never signal a reused PID. */
    }
    return pid;
}

/* The lock remains held through initialization and reaping. A disappeared
 * PID alone does not prove its retained graphics surface has been released. */
static int native_doom_busy(void) {
    if (!native_frontend) return doom_running_pid() > 0;
    char path[512];if (doom_path(path, sizeof path, "native.lock")) return 1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno != ENOENT;
    int busy = flock(fd, LOCK_EX | LOCK_NB) != 0;
    close(fd);return busy;
}

/* Stop a running engine.  Safe to call when nothing is running. */
static void doom_stop(const char *why) {
    int pid = doom_running_pid();
    if (pid < 0) return;
    jf_log("DOOM: stopping pid %d (%s)", pid, why);
    kill(pid, SIGTERM);
    for (int i = 0; i < 20 && (native_frontend ? native_doom_busy() : doom_running_pid() > 0); i++) nap(0.25);
    if (native_frontend ? native_doom_busy() : doom_running_pid() > 0) {
        if (native_frontend) { jf_log("DOOM: clean shutdown timed out; surface recovery requires inspection"); return; }
        kill(pid, SIGKILL);
        jf_log("DOOM: pid %d killed", pid);
    }
    char path[512];
    if (!doom_path(path, sizeof path, "doom.pid")) unlink(path);
}

/* Lifecycle belongs to the backend. Fixed executable and WAD, no caller paths
 * or arguments. A reaper owns the engine and removes its PID on normal exit. */
static int native_doom_start(struct sb *out) {
    char bin[512], wad[512], pidf[512], lock[512];
    doom_path(bin, sizeof bin, "bin/hr54-doom-native");
    doom_path(wad, sizeof wad, "data/doom1.wad");
    doom_path(pidf, sizeof pidf, "doom.pid");
    doom_path(lock, sizeof lock, "native.lock");
    if (!doom_armed()) return fail("Doom is disabled");
    if (doom_running_pid() > 0) { sb_puts(out, "{\"running\":true}"); return 0; }
    if (access(bin, X_OK) || access(wad, R_OK)) return fail("Native Doom assets unavailable");
    int lf = open(lock, O_CREAT | O_RDWR, 0600);
    if (lf < 0 || flock(lf, LOCK_EX | LOCK_NB)) { if (lf >= 0) close(lf); return fail("Doom lifecycle busy"); }
    int ready[2]; if (pipe(ready)) { close(lf); return fail("Doom launch pipe failed"); }
    pid_t reaper = fork();
    if (!reaper) {
        close(ready[0]); setsid(); signal(SIGCHLD, SIG_DFL); alarm(0);
        /* Do not hold the request connection while Doom runs. */
        for (int f = 3; f < 1024; f++) if (f != lf && f != ready[1]) close(f);
        int execpipe[2], initialized[2]; if (pipe(execpipe) || pipe(initialized)) _exit(1);
        fcntl(execpipe[1], F_SETFD, FD_CLOEXEC);
        pid_t engine = fork();
        if (!engine) {
            close(execpipe[0]); close(initialized[0]); close(lf); close(ready[1]);
            char fdarg[16]; snprintf(fdarg, sizeof fdarg, "%d", initialized[1]);
            execl(bin, "hr54-doom-native", "--depth", HR54_NATIVE_FOREGROUND_DEPTH_ARG, "--seconds", "0", "--ready-fd", fdarg, "-iwad", wad, (char *)NULL);
            int error = errno; write(execpipe[1], &error, sizeof error); _exit(127);
        }
        close(execpipe[1]); close(initialized[1]); int error = 0;
        ssize_t n = read(execpipe[0], &error, sizeof error); close(execpipe[0]);
        int recorded = 0;
        if (engine > 1 && n == 0) {
            FILE *p = fopen(pidf, "w");
            if (p) { fprintf(p, "%d\n", (int)engine); recorded = fclose(p) == 0; }
        }
        struct pollfd init = {initialized[0], POLLIN, 0}; unsigned char initialized_ok = 0;
        if (recorded && poll(&init, 1, 5000) > 0 && read(initialized[0], &initialized_ok, 1) == 1 && initialized_ok == 1) {
            write(ready[1], &engine, sizeof engine);
        } else if (engine > 1) kill(engine, SIGTERM);
        close(initialized[0]);
        if (engine > 1) { int status; while (waitpid(engine, &status, 0) < 0 && errno == EINTR) {} }
        unlink(pidf); close(lf); close(ready[1]); _exit(0);
    }
    close(ready[1]); close(lf);
    if (reaper < 0) { close(ready[0]); return fail("Doom launch failed"); }
    struct pollfd p = {ready[0], POLLIN, 0}; pid_t engine = 0;
    int rc = poll(&p, 1, 7000);
    ssize_t n = rc > 0 ? read(ready[0], &engine, sizeof engine) : -1; close(ready[0]);
    if (n != sizeof engine || engine <= 1) return fail("Native Doom failed to execute");
    sb_fmt(out, "{\"running\":true,\"pid\":%d}", (int)engine); return 0;
}
