/* Minimal mock native surface for the lifecycle host tests and the SDK
 * integration test. Writes the readiness byte to --ready-fd and idles until
 * SIGTERM, exactly like the real renderer's startup contract.
 *
 * When it is launched by the real service (which exports REBOX_MODULE_DATA for
 * the child) it also watches the public display and exits as soon as the
 * authoritative core publishes a committed quit, mirroring the real renderer's
 * "renderer sees quit and returns" contract. Without REBOX_MODULE_DATA it never
 * watches anything, so the pure lifecycle test is unaffected. Not shipped. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void handler(int s) { (void)s; stop = 1; }

/* Return 1 if the display bytes contain a committed quit ("quit":1). */
static int display_quit(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, "\"quit\":1") != NULL;
}

int main(int argc, char **argv) {
    int ready = -1, depth = 0;
    const char *watch = NULL;
    char watch_path[600];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ready-fd") && i + 1 < argc) ready = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--depth") && i + 1 < argc) depth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) watch = argv[++i];
    }
    (void)depth;
    if (!watch) {
        const char *data = getenv("REBOX_MODULE_DATA");
        if (data && *data) {
            snprintf(watch_path, sizeof watch_path, "%s/native/public-display.json", data);
            watch = watch_path;
        }
    }
    signal(SIGTERM, handler);
    signal(SIGINT, handler);
    if (ready >= 0) {
        unsigned char b = 1;
        if (write(ready, &b, 1) != 1) return 1;
        close(ready);
    }
    while (!stop) {
        if (watch && display_quit(watch)) break;
        struct timespec t = {0, 20000000};
        nanosleep(&t, 0);
    }
    return 0;
}
