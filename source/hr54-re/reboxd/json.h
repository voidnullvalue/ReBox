#ifndef REBOX_JSON_H
#define REBOX_JSON_H
#include <stddef.h>
struct sb {
    char *p;
    size_t len, cap;
};

enum jtype { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };

struct jval {
    enum jtype t;
    double num;
    char *str;
    struct jval **items;
    char **keys;
    size_t n, cap;
};

extern _Thread_local char g_err[256];
int fail(const char *, ...);
double mono_now(void);
void nap(double);
int write_all_fd(int, const void *, size_t);
int sb_putn(struct sb *, const char *, size_t);
int sb_puts(struct sb *, const char *);
int sb_fmt(struct sb *, const char *, ...);
int sb_json_str(struct sb *, const char *);
int url_encode(struct sb *, const char *);
int query_param(const char *, const char *, char *, size_t);
struct jval *json_parse(const char *, size_t);
void jfree(struct jval *);
struct jval *jget(struct jval *, const char *);
struct jval *jnth(struct jval *, size_t);
const char *jstr(struct jval *);
double jnum(struct jval *, double);
int jbool(struct jval *, int);
#endif
