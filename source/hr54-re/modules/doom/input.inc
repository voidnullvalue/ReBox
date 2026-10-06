static unsigned doom_input_read(void) {
    char path[512];
    if (doom_path(path, sizeof path, "input")) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned mask = 0;
    if (fscanf(f, "%u", &mask) != 1) mask = 0;
    fclose(f);
    return mask & 0xffu;
}

static void doom_input_write(unsigned mask) {
    char path[512], tmp[512];
    if (doom_path(path, sizeof path, "input")) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%u\n", mask & 0xffu);
    fclose(f);
    if (rename(tmp, path)) unlink(tmp);
}
