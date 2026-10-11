/*
 * Portable little-endian question database (JEPDB1) reader plus coherent
 * category/board selection and clue history. The binary format is small,
 * indexed and toolchain-free: the PC importer writes it with the Python
 * standard library and both the stage 2 musl service and the host tests read
 * the exact same bytes with no SQLite or other external dependency.
 */
#include "jeopardy.h"
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JDB_MAGIC "JEPDB001"
#define JDB_VERSION 1u
#define JDB_HEADER 32u
#define JDB_CAT_REC 32u
#define JDB_CLUE_REC 36u

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int read_all(const char *path, unsigned char **out, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t cap = 1 << 16, got = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) { close(fd); return -1; }
    for (;;) {
        if (got == cap) {
            if (cap > (64u << 20)) { free(buf); close(fd); return -1; }
            unsigned char *grown = realloc(buf, cap * 2);
            if (!grown) { free(buf); close(fd); return -1; }
            buf = grown; cap *= 2;
        }
        ssize_t n = read(fd, buf + got, cap - got);
        if (n < 0) { free(buf); close(fd); return -1; }
        if (!n) break;
        got += (size_t)n;
    }
    close(fd);
    *out = buf; *len = got;
    return 0;
}

static uint32_t checksum(const unsigned char *p, size_t n) {
    uint32_t v = 2166136261u;
    for (size_t i = 0; i < n; i++) v = (v ^ p[i]) * 16777619u;
    return v;
}

int jpq_db_load(JpqDb *db, const char *path) {
    if (!db) return -1;
    memset(db, 0, sizeof *db);
    unsigned char *raw = NULL;
    size_t raw_len = 0;
    if (read_all(path, &raw, &raw_len)) return -1;
    if (raw_len < JDB_HEADER + 4 || memcmp(raw, JDB_MAGIC, 8)) { free(raw); return -1; }
    if (rd32(raw + 8) != JDB_VERSION) { free(raw); return -1; }
    uint32_t cat_count = rd32(raw + 16);
    uint32_t clue_count = rd32(raw + 20);
    uint32_t string_bytes = rd32(raw + 24);
    size_t need = (size_t)JDB_HEADER + (size_t)cat_count * JDB_CAT_REC +
                  (size_t)clue_count * JDB_CLUE_REC + string_bytes + 4u;
    if (need != raw_len) { free(raw); return -1; }
    if (checksum(raw, raw_len - 4) != rd32(raw + raw_len - 4)) { free(raw); return -1; }

    const unsigned char *catp = raw + JDB_HEADER;
    const unsigned char *cluep = catp + (size_t)cat_count * JDB_CAT_REC;
    const unsigned char *strp = cluep + (size_t)clue_count * JDB_CLUE_REC;

    db->strings = malloc(string_bytes ? string_bytes : 1);
    db->cats = cat_count ? calloc(cat_count, sizeof *db->cats) : NULL;
    db->clues = clue_count ? calloc(clue_count, sizeof *db->clues) : NULL;
    if (!db->strings || (cat_count && !db->cats) || (clue_count && !db->clues)) {
        free(raw); jpq_db_free(db); return -1;
    }
    memcpy(db->strings, strp, string_bytes);
    db->strings_len = string_bytes;
    db->cat_count = cat_count;
    db->clue_count = clue_count;

    for (uint32_t i = 0; i < cat_count; i++) {
        const unsigned char *r = catp + (size_t)i * JDB_CAT_REC;
        JpqCategory *c = &db->cats[i];
        uint32_t off = rd32(r + 0);
        uint16_t len = rd16(r + 4);
        c->round = r[6];
        c->difficulty = r[7];
        c->year = rd32(r + 8);
        c->episode = rd32(r + 12);
        c->airdate = rd32(r + 16);
        c->first_clue = rd32(r + 20);
        c->clue_count = rd16(r + 24);
        if ((size_t)off + len > string_bytes || !len) { free(raw); jpq_db_free(db); return -1; }
        c->name = malloc(len + 1);
        if (!c->name) { free(raw); jpq_db_free(db); return -1; }
        memcpy(c->name, db->strings + off, len);
        c->name[len] = 0;
        if ((size_t)c->first_clue + c->clue_count > clue_count) { free(raw); jpq_db_free(db); return -1; }
    }
    for (uint32_t i = 0; i < clue_count; i++) {
        const unsigned char *r = cluep + (size_t)i * JDB_CLUE_REC;
        JpqClue *q = &db->clues[i];
        q->category = rd32(r + 0);
        q->value = rd32(r + 4);
        q->episode = rd32(r + 8);
        q->airdate = rd32(r + 12);
        q->round = r[16];
        q->difficulty = r[17];
        q->flags = r[18];
        uint32_t coff = rd32(r + 20);
        uint16_t clen = rd16(r + 24);
        uint32_t roff = rd32(r + 28);
        uint16_t rlen = rd16(r + 32);
        if (q->category >= cat_count) { free(raw); jpq_db_free(db); return -1; }
        if ((size_t)coff + clen > string_bytes || (size_t)roff + rlen > string_bytes) {
            free(raw); jpq_db_free(db); return -1;
        }
        q->clue = db->strings + coff; q->clue_len = clen;
        q->response = db->strings + roff; q->resp_len = rlen;
    }
    free(raw);
    return 0;
}

void jpq_db_free(JpqDb *db) {
    if (!db) return;
    if (db->cats) {
        for (uint32_t i = 0; i < db->cat_count; i++) free(db->cats[i].name);
        free(db->cats);
    }
    free(db->clues);
    free(db->strings);
    memset(db, 0, sizeof *db);
}

const char *jpq_db_clue_text(const JpqDb *db, uint32_t clue) {
    if (!db || clue >= db->clue_count) return "";
    return db->clues[clue].clue;
}
const char *jpq_db_clue_response(const JpqDb *db, uint32_t clue) {
    if (!db || clue >= db->clue_count) return "";
    return db->clues[clue].response;
}

/* ------------------------------------------------------------------ */
/* History                                                             */
/* ------------------------------------------------------------------ */

int jpq_history_init(JpqHistory *h, uint32_t clue_count) {
    if (!h) return -1;
    h->count = clue_count;
    h->bits = clue_count ? calloc((clue_count + 7) / 8, 1) : NULL;
    return (clue_count && !h->bits) ? -1 : 0;
}
void jpq_history_free(JpqHistory *h) {
    if (!h) return;
    free(h->bits);
    h->bits = NULL;
    h->count = 0;
}
void jpq_history_reset(JpqHistory *h) {
    if (h && h->bits) memset(h->bits, 0, (h->count + 7) / 8);
}
int jpq_history_used(const JpqHistory *h, uint32_t clue) {
    if (!h || !h->bits || clue >= h->count) return 0;
    return (h->bits[clue >> 3] >> (clue & 7)) & 1;
}
void jpq_history_mark(JpqHistory *h, uint32_t clue) {
    if (h && h->bits && clue < h->count) h->bits[clue >> 3] |= (uint8_t)(1u << (clue & 7));
}

/* ------------------------------------------------------------------ */
/* Selection                                                           */
/* ------------------------------------------------------------------ */

static uint32_t xs(uint32_t *s) {
    uint32_t x = *s ? *s : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}
static uint32_t rng_below(uint32_t *s, uint32_t n) {
    return n ? xs(s) % n : 0;
}

static int cat_matches(const JpqCategory *c, uint32_t idx, const JpqFilter *f) {
    if (!f) return 1;
    if (f->year_min && c->year < f->year_min) return 0;
    if (f->year_max && c->year > f->year_max) return 0;
    if (f->difficulty_min > 0 && c->difficulty < (uint8_t)f->difficulty_min) return 0;
    if (f->difficulty_max > 0 && c->difficulty > (uint8_t)f->difficulty_max) return 0;
    if (f->category >= 0 && (uint32_t)f->category != idx) return 0;
    if (f->episode && c->episode != f->episode) return 0;
    return 1;
}

int jpq_db_count(const JpqDb *db, const JpqFilter *f) {
    int n = 0;
    if (!db) return 0;
    for (uint32_t i = 0; i < db->cat_count; i++)
        if (cat_matches(&db->cats[i], i, f)) n++;
    return n;
}

static int cat_is_fresh(const JpqDb *db, const JpqHistory *h, uint32_t ci) {
    const JpqCategory *c = &db->cats[ci];
    for (uint16_t k = 0; k < c->clue_count; k++)
        if (jpq_history_used(h, c->first_clue + k)) return 0;
    return 1;
}

/* Gather candidate category indices for a round under a filter. */
static uint32_t gather(const JpqDb *db, const JpqHistory *h, const JpqFilter *f,
                       uint8_t round, int require_fresh, int *out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < db->cat_count; i++) {
        const JpqCategory *c = &db->cats[i];
        if (c->round != round) continue;
        if (round == JPQ_ROUND_J || round == JPQ_ROUND_DJ) {
            if (c->clue_count < JPQ_ROWS) continue;
        } else if (c->clue_count < 1) continue;
        if (f && f->category >= 0 && (uint32_t)f->category != i) continue;
        if (!cat_matches(c, i, f)) continue;
        if (require_fresh && !cat_is_fresh(db, h, i)) continue;
        if (n < cap) out[n] = (int)i;
        n++;
    }
    return n;
}

/* Copy a category's five clues into a board column, ordered by original value.
 * Gameplay uses the authentic current amounts for the round (single Jeopardy
 * $200..$1000, Double Jeopardy $400..$2000); the database keeps each clue's
 * original broadcast value untouched in JpqClue.value. */
static void fill_column(const JpqDb *db, JpqHistory *h, JpqBoard *b, int col, uint32_t ci, uint8_t round) {
    const JpqCategory *c = &db->cats[ci];
    uint32_t ids[JPQ_ROWS];
    uint16_t n = c->clue_count < JPQ_ROWS ? c->clue_count : JPQ_ROWS;
    for (uint16_t k = 0; k < n; k++) ids[k] = c->first_clue + k;
    /* insertion sort by original value; keeps values authentic per clue */
    for (uint16_t a = 1; a < n; a++) {
        uint32_t v = ids[a]; uint16_t j = a;
        while (j > 0 && db->clues[ids[j - 1]].value > db->clues[v].value) { ids[j] = ids[j - 1]; j--; }
        ids[j] = v;
    }
    b->cat[col] = (int32_t)ci;
    int32_t base = (round == JPQ_ROUND_DJ) ? 400 : 200;
    for (uint16_t row = 0; row < JPQ_ROWS; row++) {
        if (row < n) {
            b->clue[col][row] = (int32_t)ids[row];
            b->value[col][row] = base * (int32_t)(row + 1);
            jpq_history_mark(h, ids[row]);
        } else {
            b->clue[col][row] = -1;
            b->value[col][row] = 0;
        }
    }
}

/* Place the requested daily doubles, preferring the historical locations
 * recorded on the selected clues before falling back to a random row-2..5
 * cell (an authentic board never uses the top row). */
static void place_dailies(const JpqDb *db, JpqBoard *b, uint8_t count, uint32_t *rng) {
    b->dd_count = 0;
    if (!count) return;
    if (count > JPQ_MAX_DD) count = JPQ_MAX_DD;
    uint8_t used[JPQ_COLS][JPQ_ROWS];
    memset(used, 0, sizeof used);
    int32_t flagged[JPQ_COLS * JPQ_ROWS][2];
    int nf = 0;
    for (int col = 0; col < JPQ_COLS; col++)
        for (int row = 1; row < JPQ_ROWS; row++) {
            int32_t cid = b->clue[col][row];
            if (cid < 0 || (uint32_t)cid >= db->clue_count) continue;
            if (db->clues[cid].flags & JPQ_QFLAG_DD) { flagged[nf][0] = col; flagged[nf][1] = row; nf++; }
        }
    for (int i = nf; i > 1; i--) {
        int j = (int)rng_below(rng, (uint32_t)i);
        int32_t t0 = flagged[i - 1][0], t1 = flagged[i - 1][1];
        flagged[i - 1][0] = flagged[j][0]; flagged[i - 1][1] = flagged[j][1];
        flagged[j][0] = t0; flagged[j][1] = t1;
    }
    for (int i = 0; i < nf && b->dd_count < count; i++) {
        int col = flagged[i][0], row = flagged[i][1];
        if (used[col][row]) continue;
        used[col][row] = 1;
        b->dd_col[b->dd_count] = (uint8_t)col;
        b->dd_row[b->dd_count] = (uint8_t)row;
        b->dd_count++;
    }
    while (b->dd_count < count) {
        uint8_t col = 0, row = 0;
        int found = 0;
        for (int tries = 0; tries < 64; tries++) {
            col = (uint8_t)rng_below(rng, JPQ_COLS);
            row = (uint8_t)(1 + rng_below(rng, JPQ_ROWS - 1));
            if (!used[col][row]) { found = 1; break; }
        }
        if (!found) break;
        used[col][row] = 1;
        b->dd_col[b->dd_count] = col;
        b->dd_row[b->dd_count] = row;
        b->dd_count++;
    }
}

int jpq_build_round(JpqGame *g, const JpqFilter *f, uint8_t round, uint32_t seed, JpqBoard *out) {
    if (!g || !g->db || !g->history || !out) return -1;
    JpqDb *db = g->db;
    JpqHistory *h = g->history;
    int *cand = malloc(sizeof(int) * (db->cat_count ? db->cat_count : 1));
    if (!cand) return -1;
    uint32_t n = gather(db, h, f, round, 1, cand, db->cat_count);
    if (n < JPQ_COLS) {
        /* The fresh pool for *this round alone* is exhausted. Recycle only
         * this round's clues: a global jpq_history_reset() would also erase
         * the other rounds' progress and let already-played clues repeat. */
        n = gather(db, h, f, round, 0, cand, db->cat_count);
    }
    if (n < JPQ_COLS) { free(cand); return -1; }

    memset(out, 0, sizeof *out);
    out->round = round;
    out->final_clue = -1;
    uint32_t rng = seed ? seed : 0x1234567u;
    int chosen[JPQ_COLS];

    if (f && f->episode) {
        /* keep the whole board from one broadcast episode */
        int ep[JPQ_COLS]; uint32_t en = 0;
        for (uint32_t i = 0; i < n && en < JPQ_COLS; i++)
            if (db->cats[cand[i]].episode == f->episode) ep[en++] = cand[i];
        if (en < JPQ_COLS) {
            /* An explicit episode must be complete for this round. Never
             * silently substitute unrelated random categories. */
            free(cand);
            return -1;
        }
        for (uint32_t i = en; i > 1; i--) { uint32_t j = rng_below(&rng, i); int t = ep[i - 1]; ep[i - 1] = ep[j]; ep[j] = t; }
        for (int i = 0; i < JPQ_COLS; i++) chosen[i] = ep[i];
    } else if (g->cfg.random_categories) {
        for (uint32_t i = n; i > 1; i--) { uint32_t j = rng_below(&rng, i); int t = cand[i - 1]; cand[i - 1] = cand[j]; cand[j] = t; }
        for (int i = 0; i < JPQ_COLS; i++) chosen[i] = cand[i];
    } else {
        /* pick a single episode that owns at least six eligible categories */
        uint32_t eps[256]; uint32_t epc[256]; uint32_t en = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t e = db->cats[cand[i]].episode, k;
            for (k = 0; k < en; k++) if (eps[k] == e) break;
            if (k == en && en < 256) { eps[en] = e; epc[en] = 0; en++; }
            if (k < en) epc[k]++;
        }
        uint32_t pick = 0; int have = 0;
        for (uint32_t i = 0; i < en; i++) if (epc[i] >= JPQ_COLS && (!have || rng_below(&rng, 2))) { pick = i; have = 1; }
        if (have) {
            uint32_t c = 0;
            for (uint32_t i = 0; i < n && c < JPQ_COLS; i++)
                if (db->cats[cand[i]].episode == eps[pick]) chosen[c] = cand[i], c++;
        } else {
            for (int i = 0; i < JPQ_COLS; i++) chosen[i] = cand[i];
        }
    }
    for (int col = 0; col < JPQ_COLS; col++) fill_column(db, h, out, col, (uint32_t)chosen[col], round);
    uint8_t dd = (round == JPQ_ROUND_J) ? g->cfg.dd_count_j : g->cfg.dd_count_dj;
    place_dailies(db, out, dd, &rng);
    g->rng = rng;
    free(cand);
    return 0;
}

int jpq_build_final(JpqGame *g, const JpqFilter *f, uint32_t seed, int32_t *clue_out) {
    if (!g || !g->db || !g->history) return -1;
    JpqDb *db = g->db;
    JpqHistory *h = g->history;
    int *cand = malloc(sizeof(int) * (db->cat_count ? db->cat_count : 1));
    if (!cand) return -1;
    uint32_t n = gather(db, h, f, JPQ_ROUND_FINAL, 1, cand, db->cat_count);
    /* The Final pool may simply be exhausted while the J/DJ pools are not:
     * reuse already-used Final clues without wiping the shared history. */
    if (!n) n = gather(db, h, f, JPQ_ROUND_FINAL, 0, cand, db->cat_count);
    if (!n) { free(cand); return -1; }
    uint32_t rng = seed ? seed : 0x89abcdefu;
    uint32_t pick = (uint32_t)cand[rng_below(&rng, n)];
    int32_t clue = (int32_t)db->cats[pick].first_clue;
    jpq_history_mark(h, (uint32_t)clue);
    g->rng = rng;
    free(cand);
    if (clue_out) *clue_out = clue;
    return 0;
}

/* Choose one complete historical episode that owns a full single and double
 * board plus at least one Final clue, so all three rounds share a broadcast. */
static int choose_episode(const JpqDb *db, const JpqFilter *f, const JpqHistory *h,
                          uint32_t seed, uint32_t *ep_out) {
    if (!db || !db->cat_count) return -1;
    uint32_t cap = db->cat_count;
    uint32_t *eps = calloc(cap, sizeof *eps);
    uint32_t *jc = calloc(cap, sizeof *jc), *dc = calloc(cap, sizeof *dc), *fc = calloc(cap, sizeof *fc);
    uint32_t *fjc = calloc(cap, sizeof *fjc), *fdc = calloc(cap, sizeof *fdc), *ffc = calloc(cap, sizeof *ffc);
    int *viable = calloc(cap, sizeof *viable);
    if (!eps || !jc || !dc || !fc || !fjc || !fdc || !ffc || !viable) {
        free(eps); free(jc); free(dc); free(fc); free(fjc); free(fdc); free(ffc); free(viable);
        return -1;
    }
    uint32_t en = 0;
    for (uint32_t i = 0; i < db->cat_count; i++) {
        const JpqCategory *c = &db->cats[i];
        if (!cat_matches(c, i, f)) continue;
        uint32_t k;
        for (k = 0; k < en; k++) if (eps[k] == c->episode) break;
        if (k == en) { if (en >= cap) break; eps[en] = c->episode; en++; }
        int fresh = cat_is_fresh(db, h, i);
        if (c->round == JPQ_ROUND_J && c->clue_count >= JPQ_ROWS) { jc[k]++; if (fresh) fjc[k]++; }
        else if (c->round == JPQ_ROUND_DJ && c->clue_count >= JPQ_ROWS) { dc[k]++; if (fresh) fdc[k]++; }
        else if (c->round == JPQ_ROUND_FINAL && c->clue_count >= 1) { fc[k]++; if (fresh) ffc[k]++; }
    }
    /* Prefer complete episodes whose boards are still entirely unplayed so a
     * fresh broadcast is chosen while one remains; only then recycle. */
    uint32_t nv = 0;
    for (uint32_t k = 0; k < en; k++)
        if (fjc[k] >= JPQ_COLS && fdc[k] >= JPQ_COLS && ffc[k] >= 1) viable[nv++] = (int)k;
    if (!nv)
        for (uint32_t k = 0; k < en; k++)
            if (jc[k] >= JPQ_COLS && dc[k] >= JPQ_COLS && fc[k] >= 1) viable[nv++] = (int)k;
    int rc = -1;
    if (nv) {
        uint32_t rng = seed ? seed : 0x1234567u;
        *ep_out = eps[(uint32_t)viable[rng_below(&rng, nv)]];
        rc = 0;
    }
    free(eps); free(jc); free(dc); free(fc); free(fjc); free(fdc); free(ffc); free(viable);
    return rc;
}

int jpq_build_boards(JpqGame *g, uint32_t seed) {
    if (!g || !g->db || !g->history) return -1;
    JpqFilter f;
    int have = 0;
    if (g->filter_set) { f = g->filter; have = 1; }
    if (have && f.episode) {
        /* explicit episode: the build only succeeds for a complete episode */
    } else if (!g->cfg.random_categories) {
        uint32_t ep = 0;
        if (choose_episode(g->db, have ? &f : NULL, g->history, seed, &ep) != 0) {
            /* episode mode with no complete episode must fail, not degrade
             * into unrelated random categories */
            return -1;
        }
        if (!have) { memset(&f, 0, sizeof f); f.category = -1; }
        f.episode = ep;
        have = 1;
    }
    const JpqFilter *fp = have ? &f : NULL;

    JpqBoard bj, bdj;
    int32_t fc = -1;
    size_t hbytes = (g->history->count + 7) / 8;
    uint8_t *hsave = hbytes ? malloc(hbytes) : NULL;
    if (hbytes && !hsave) return -1;
    if (hsave) memcpy(hsave, g->history->bits, hbytes);
    uint32_t rng_before = g->rng;
    int ok = jpq_build_round(g, fp, JPQ_ROUND_J, seed, &bj) == 0 &&
             jpq_build_round(g, fp, JPQ_ROUND_DJ, seed ^ 0x5bd1e995u, &bdj) == 0 &&
             jpq_build_final(g, fp, seed ^ 0x27d4eb2fu, &fc) == 0;
    if (!ok) {
        if (hsave) memcpy(g->history->bits, hsave, hbytes);
        g->rng = rng_before;
        free(hsave);
        return -1;
    }
    free(hsave);
    g->board_j = bj;
    g->board_dj = bdj;
    g->final_clue = fc;
    /* Never write the randomly chosen episode back into the configured filter:
     * that pinned the same exhausted episode across games. The board records
     * the chosen categories; g->filter stays exactly as the lobby configured. */
    return 0;
}

void jpq_reset_history(JpqGame *g) {
    if (g && g->history) jpq_history_reset(g->history);
}
