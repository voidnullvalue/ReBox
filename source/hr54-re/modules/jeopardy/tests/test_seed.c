/* Host test for the shared SDK default-seeding used by the Jeopardy package:
 * the bundled demo questions are copied into private data once, with safe
 * permissions, and an existing user file is never replaced. */
#include "../../shared/sdk.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite(data, 1, strlen(data), f);
    fclose(f);
}

static char *read_file(const char *path) {
    static char buf[256];
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

int main(void) {
    char pkg[] = "/tmp/jpq-seed-pkg-XXXXXX", data[] = "/tmp/jpq-seed-data-XXXXXX";
    assert(mkdtemp(pkg) && mkdtemp(data));
    char dir[512], file[640];
    snprintf(dir, sizeof dir, "%s/default-data", pkg);
    assert(mkdir(dir, 0700) == 0);
    snprintf(file, sizeof file, "%s/questions.jdb", dir);
    write_file(file, "JEPDB1-default");
    setenv("REBOX_MODULE_PACKAGE", pkg, 1);
    setenv("REBOX_MODULE_DATA", data, 1);

    assert(rb_module_seed("default-data", "") == 0);
    char seeded[640];
    snprintf(seeded, sizeof seeded, "%s/questions.jdb", data);
    struct stat st;
    assert(stat(seeded, &st) == 0);
    assert((st.st_mode & 0777) == 0600);           /* data file permission */
    assert(strcmp(read_file(seeded), "JEPDB1-default") == 0);

    /* A second run must not replace an existing (possibly user-edited) file. */
    write_file(seeded, "USER-EDITED");
    assert(rb_module_seed("default-data", "") == 0);
    assert(strcmp(read_file(seeded), "USER-EDITED") == 0);

    unlink(seeded);
    unlink(file);
    rmdir(dir);
    rmdir(pkg);
    rmdir(data);
    puts("seed: defaults copied 0600, existing data preserved");
    return 0;
}
