#include "jpq_parse.h"

static int newnode(JpqJson *J) {
    if (J->n >= JPQ_JSON_NODES) return -1;
    JpqNode *n = &J->nodes[J->n];
    n->type = JPQ_JNULL; n->num = 0; n->str_off = 0; n->str_len = 0;
    n->key_off = 0; n->key_len = 0;
    n->first = -1; n->next = -1; n->nchildren = 0;
    return (int)J->n++;
}

static void skip_ws(const char **p, const char *end) {
    while (*p < end && (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r')) (*p)++;
}

static int hex4(const char **p, const char *end, unsigned *out) {
    if (end - *p < 4) return -1;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = (*p)[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *p += 4;
    *out = v;
    return 0;
}

static int emit_utf8(JpqJson *J, unsigned cp, uint32_t *off, uint32_t *len) {
    char t[4];
    int n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { t[0] = (char)(0xF0 | (cp >> 18)); t[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); t[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    if (J->pool_used + (uint32_t)n + 1 > JPQ_JSON_POOL) return -1;
    *off = J->pool_used;
    for (int i = 0; i < n; i++) J->pool[J->pool_used++] = t[i];
    J->pool[J->pool_used++] = 0;
    *len = (uint32_t)n;
    return 0;
}

/* Parse a string body; opening quote is at *p. Records offset/len in the pool
 * but the terminating NUL is shared (full strings are NUL-terminated). */
static int parse_string(JpqJson *J, const char **p, const char *end, uint32_t *off, uint32_t *len) {
    if (*p >= end || **p != '"') return -1;
    (*p)++;
    *off = J->pool_used;
    uint32_t n = 0;
    while (*p < end) {
        char c = **p;
        if (c == '"') {
            (*p)++;
            if (J->pool_used + 1 > JPQ_JSON_POOL) return -1;
            J->pool[J->pool_used++] = 0;
            *len = n;
            return 0;
        }
        if ((unsigned char)c < 0x20) return -1;
        if (c == '\\') {
            (*p)++;
            if (*p >= end) return -1;
            char e = **p;
            (*p)++;
            char out;
            switch (e) {
            case '"': out = '"'; break;
            case '\\': out = '\\'; break;
            case '/': out = '/'; break;
            case 'b': out = '\b'; break;
            case 'f': out = '\f'; break;
            case 'n': out = '\n'; break;
            case 'r': out = '\r'; break;
            case 't': out = '\t'; break;
            case 'u': {
                unsigned cp;
                if (hex4(p, end, &cp)) return -1;
                if (cp >= 0xD800 && cp <= 0xDBFF && end - *p >= 6 && (*p)[0] == '\\' && (*p)[1] == 'u') {
                    const char *save = *p; unsigned lo; *p += 2;
                    if (!hex4(p, end, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else *p = save;
                }
                if (!cp || (cp >= 0xD800 && cp <= 0xDFFF)) return -1;
                uint32_t uo, ul;
                if (emit_utf8(J, cp, &uo, &ul)) return -1;
                n += ul;
                continue;
            }
            default: return -1;
            }
            if (J->pool_used + 1 > JPQ_JSON_POOL) return -1;
            J->pool[J->pool_used++] = out;
            n++;
            continue;
        }
        if (J->pool_used + 1 > JPQ_JSON_POOL) return -1;
        J->pool[J->pool_used++] = c;
        n++;
        (*p)++;
    }
    return -1;
}

static int parse_value(JpqJson *J, const char **p, const char *end, int depth);

static int append_child(JpqJson *J, int parent, int child) {
    J->nodes[child].next = -1;
    if (J->nodes[parent].first < 0) J->nodes[parent].first = child;
    else {
        int t = J->nodes[parent].first;
        while (J->nodes[t].next >= 0) t = J->nodes[t].next;
        J->nodes[t].next = child;
    }
    J->nodes[parent].nchildren++;
    return 0;
}

static int keys_equal(const JpqJson *J, const JpqNode *n, const char *key) {
    const char *k = J->pool + n->key_off;
    for (uint32_t i = 0; i < n->key_len; i++) if (k[i] != key[i]) return 0;
    return key[n->key_len] == 0;
}

static int parse_object(JpqJson *J, const char **p, const char *end, int depth) {
    int node = newnode(J);
    if (node < 0) return -1;
    J->nodes[node].type = JPQ_JOBJ;
    (*p)++; /* '{' */
    skip_ws(p, end);
    if (*p < end && **p == '}') { (*p)++; return node; }
    for (;;) {
        skip_ws(p, end);
        uint32_t ko = 0, kl = 0;
        if (parse_string(J, p, end, &ko, &kl)) return -1;
        if (kl > JPQ_JSON_KEYMAX) return -1;
        skip_ws(p, end);
        if (*p >= end || **p != ':') return -1;
        (*p)++;
        int child = parse_value(J, p, end, depth + 1);
        if (child < 0) return -1;
        J->nodes[child].key_off = ko;
        J->nodes[child].key_len = kl;
        /* duplicate key rejected */
        for (int t = J->nodes[node].first; t >= 0; t = J->nodes[t].next)
            if (keys_equal(J, &J->nodes[t], J->pool + ko)) return -1;
        append_child(J, node, child);
        skip_ws(p, end);
        if (*p < end && **p == ',') { (*p)++; continue; }
        if (*p < end && **p == '}') { (*p)++; return node; }
        return -1;
    }
}

static int parse_array(JpqJson *J, const char **p, const char *end, int depth) {
    int node = newnode(J);
    if (node < 0) return -1;
    J->nodes[node].type = JPQ_JARR;
    (*p)++; /* '[' */
    skip_ws(p, end);
    if (*p < end && **p == ']') { (*p)++; return node; }
    for (;;) {
        int child = parse_value(J, p, end, depth + 1);
        if (child < 0) return -1;
        append_child(J, node, child);
        skip_ws(p, end);
        if (*p < end && **p == ',') { (*p)++; continue; }
        if (*p < end && **p == ']') { (*p)++; return node; }
        return -1;
    }
}

static int parse_number(const char **p, const char *end, double *out) {
    const char *s = *p;
    int neg = 0;
    if (s < end && *s == '-') { neg = 1; s++; }
    if (s >= end) return -1;
    double v = 0;
    if (*s == '0') s++;
    else if (*s >= '1' && *s <= '9') { while (s < end && *s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); }
    else return -1;
    if (s < end && *s == '.') {
        s++;
        double scale = 0.1;
        if (s >= end || *s < '0' || *s > '9') return -1;
        while (s < end && *s >= '0' && *s <= '9') { v += (*s++ - '0') * scale; scale *= 0.1; }
    }
    if (s < end && (*s == 'e' || *s == 'E')) {
        s++;
        int esign = 1;
        if (s < end && (*s == '+' || *s == '-')) { if (*s == '-') esign = -1; s++; }
        if (s >= end || *s < '0' || *s > '9') return -1;
        int e = 0;
        while (s < end && *s >= '0' && *s <= '9') { if (e < 1000) e = e * 10 + (*s - '0'); s++; }
        while (e-- > 0) v = esign > 0 ? v * 10 : v / 10;
    }
    *out = neg ? -v : v;
    *p = s;
    return 0;
}

static int parse_value(JpqJson *J, const char **p, const char *end, int depth) {
    if (depth > JPQ_JSON_DEPTH) return -1;
    skip_ws(p, end);
    if (*p >= end) return -1;
    char c = **p;
    if (c == '{') return parse_object(J, p, end, depth);
    if (c == '[') return parse_array(J, p, end, depth);
    if (c == '"') {
        int node = newnode(J);
        if (node < 0) return -1;
        J->nodes[node].type = JPQ_JSTR;
        if (parse_string(J, p, end, &J->nodes[node].str_off, &J->nodes[node].str_len)) return -1;
        return node;
    }
    if (end - *p >= 4 && (*p)[0] == 't' && (*p)[1] == 'r' && (*p)[2] == 'u' && (*p)[3] == 'e') { *p += 4; int n = newnode(J); if (n < 0) return -1; J->nodes[n].type = JPQ_JBOOL; J->nodes[n].num = 1; return n; }
    if (end - *p >= 5 && (*p)[0] == 'f' && (*p)[1] == 'a' && (*p)[2] == 'l' && (*p)[3] == 's' && (*p)[4] == 'e') { *p += 5; int n = newnode(J); if (n < 0) return -1; J->nodes[n].type = JPQ_JBOOL; J->nodes[n].num = 0; return n; }
    if (end - *p >= 4 && (*p)[0] == 'n' && (*p)[1] == 'u' && (*p)[2] == 'l' && (*p)[3] == 'l') { *p += 4; int n = newnode(J); if (n < 0) return -1; J->nodes[n].type = JPQ_JNULL; return n; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        double v;
        if (parse_number(p, end, &v)) return -1;
        int n = newnode(J);
        if (n < 0) return -1;
        J->nodes[n].type = JPQ_JNUM;
        J->nodes[n].num = v;
        return n;
    }
    return -1;
}

int jpq_json_parse(JpqJson *J, const char *text, size_t len, int *out_root) {
    if (!J || !text || !out_root || len > 2u * 1024 * 1024) return -1;
    J->n = 0;
    J->pool_used = 0;
    const char *p = text, *end = text + len;
    int root = parse_value(J, &p, end, 0);
    if (root < 0) return -1;
    skip_ws(&p, end);
    if (p != end) return -1;
    J->root = root;
    *out_root = root;
    return 0;
}

const JpqNode *jpq_json_get(const JpqJson *J, int node, const char *key) {
    if (node < 0 || (uint32_t)node >= J->n || !key) return 0;
    const JpqNode *n = &J->nodes[node];
    if (n->type != JPQ_JOBJ) return 0;
    for (int t = n->first; t >= 0; t = J->nodes[t].next)
        if (keys_equal(J, &J->nodes[t], key)) return &J->nodes[t];
    return 0;
}

const JpqNode *jpq_json_at(const JpqJson *J, int node, uint32_t index) {
    if (node < 0 || (uint32_t)node >= J->n) return 0;
    const JpqNode *n = &J->nodes[node];
    if (n->type != JPQ_JARR || index >= n->nchildren) return 0;
    int t = n->first;
    for (uint32_t i = 0; i < index && t >= 0; i++) t = J->nodes[t].next;
    return t < 0 ? 0 : &J->nodes[t];
}

const char *jpq_json_str(const JpqJson *J, const JpqNode *n) {
    if (!n || n->type != JPQ_JSTR) return 0;
    return J->pool + n->str_off;
}

double jpq_json_num(const JpqNode *n, double dflt) { return n && n->type == JPQ_JNUM ? n->num : dflt; }
int jpq_json_bool(const JpqNode *n, int dflt) {
    if (!n) return dflt;
    if (n->type == JPQ_JBOOL) return n->num != 0;
    if (n->type == JPQ_JNULL) return 0;
    return dflt;
}
int jpq_json_count(const JpqJson *J, int node) {
    if (node < 0 || (uint32_t)node >= J->n) return 0;
    return (int)J->nodes[node].nchildren;
}
