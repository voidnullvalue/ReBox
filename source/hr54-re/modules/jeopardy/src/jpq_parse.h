#ifndef JPQ_PARSE_H
#define JPQ_PARSE_H
/* Bounded, allocation-free JSON reader for the native renderer.
 *
 * The shared service parser (reboxd/json.c) is a general malloc DOM. The
 * receiver renderer is built freestanding against minimal vendor headers, so it
 * uses this small arena parser instead: a fixed node pool, a fixed string pool,
 * a hard depth cap and manual number scanning. It parses the redacted
 * public-display.json and the private metadata.json only. */
#include <stddef.h>
#include <stdint.h>

enum { JPQ_JNULL, JPQ_JBOOL, JPQ_JNUM, JPQ_JSTR, JPQ_JARR, JPQ_JOBJ };

#define JPQ_JSON_NODES 4096
#define JPQ_JSON_POOL 131072
#define JPQ_JSON_DEPTH 24
#define JPQ_JSON_KEYMAX 64

typedef struct {
    uint8_t type;
    double num;
    uint32_t str_off, str_len;   /* string value */
    uint32_t key_off, key_len;   /* object member key */
    int32_t first, next;         /* child chain for arrays/objects */
    uint32_t nchildren;
} JpqNode;

typedef struct {
    JpqNode nodes[JPQ_JSON_NODES];
    uint32_t n;
    char pool[JPQ_JSON_POOL];
    uint32_t pool_used;
    int root;
} JpqJson;

/* Returns 0 and sets *out on success; -1 on malformed/oversized/duplicate-key
 * input. The parser never dereferences beyond [text, text+len). */
int jpq_json_parse(JpqJson *, const char *text, size_t len, int *out_root);
const JpqNode *jpq_json_get(const JpqJson *, int node, const char *key);
const JpqNode *jpq_json_at(const JpqJson *, int node, uint32_t index);
const char *jpq_json_str(const JpqJson *, const JpqNode *);
double jpq_json_num(const JpqNode *, double dflt);
int jpq_json_bool(const JpqNode *, int dflt);
int jpq_json_count(const JpqJson *, int node);
#endif
