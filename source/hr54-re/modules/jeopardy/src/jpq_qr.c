#include "jpq_qr.h"

/* GF(256) with primitive polynomial 0x11D, generator 2. */
static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static int gf_ready;

static void gf_init(void) {
    if (gf_ready) return;
    int x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    for (int i = 255; i < 512; i++) gf_exp[i] = gf_exp[i - 255];
    gf_ready = 1;
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (!a || !b) return 0;
    return gf_exp[gf_log[a] + gf_log[b]];
}

/* Reed-Solomon generator polynomial for `degree` check bytes.
 * gen has degree+1 coefficients, gen[0] is the leading (x^degree) term. */
static void rs_generator(int degree, uint8_t *gen) {
    uint8_t out[64];
    for (int i = 0; i < 64; i++) out[i] = 0;
    out[0] = 1;
    int n = 0; /* current degree */
    for (int i = 0; i < degree; i++) {
        uint8_t a = gf_exp[i];
        uint8_t res[64];
        for (int k = 0; k < 64; k++) res[k] = 0;
        for (int k = 0; k <= n; k++) {
            res[k] ^= gf_mul(out[k], a); /* a * out[k] */
            res[k + 1] ^= out[k];        /* x * out[k] */
        }
        for (int k = 0; k < 64; k++) out[k] = res[k];
        n++;
    }
    for (int i = 0; i <= degree; i++) gen[i] = out[degree - i]; /* leading term first */
}

/* Long division remainder (the check bytes). */
static void rs_encode(const uint8_t *data, int len, int ecc, uint8_t *check) {
    uint8_t gen[64];
    rs_generator(ecc, gen);
    uint8_t rem[64];
    for (int i = 0; i < ecc; i++) rem[i] = 0;
    for (int i = 0; i < len; i++) {
        uint8_t factor = data[i] ^ rem[0];
        for (int j = 0; j < ecc - 1; j++) rem[j] = rem[j + 1];
        rem[ecc - 1] = 0;
        for (int j = 0; j < ecc; j++) rem[j] ^= gf_mul(gen[j + 1], factor);
    }
    for (int i = 0; i < ecc; i++) check[i] = rem[i];
}

typedef struct { int version, size, data, ecc; int align[4], nalign; } QrVer;
static const QrVer versions[] = {
    {1, 21, 19, 7, {0}, 0},
    {2, 25, 34, 10, {6, 18}, 2},
    {3, 29, 55, 15, {6, 22}, 2},
    {4, 33, 80, 20, {6, 26}, 2},
};

static int choose_version(size_t len, const QrVer **out) {
    for (size_t i = 0; i < sizeof versions / sizeof versions[0]; i++) {
        int cap = (versions[i].data * 8 - 12) / 8;
        if ((int)len <= cap) { *out = &versions[i]; return 0; }
    }
    return -1;
}

static void set_func(uint8_t func[JPQ_QR_MAX][JPQ_QR_MAX], int r, int c) { func[r][c] = 1; }

static void draw_finder(uint8_t m[JPQ_QR_MAX][JPQ_QR_MAX], uint8_t func[JPQ_QR_MAX][JPQ_QR_MAX], int r0, int c0) {
    for (int r = -1; r <= 7; r++) for (int c = -1; c <= 7; c++) {
        int rr = r0 + r, cc = c0 + c;
        if (rr < 0 || cc < 0 || rr >= JPQ_QR_MAX || cc >= JPQ_QR_MAX) continue;
        set_func(func, rr, cc);
        if (r < 0 || r > 6 || c < 0 || c > 6) { m[rr][cc] = 0; continue; }
        int ring = (r == 0 || r == 6 || c == 0 || c == 6);
        int core = (r >= 2 && r <= 4 && c >= 2 && c <= 4);
        m[rr][cc] = (ring || core) ? 1 : 0;
    }
}

static void setup_function(const QrVer *v, uint8_t m[JPQ_QR_MAX][JPQ_QR_MAX], uint8_t func[JPQ_QR_MAX][JPQ_QR_MAX]) {
    int size = v->size;
    for (int r = 0; r < size; r++) for (int c = 0; c < size; c++) { m[r][c] = 0; func[r][c] = 0; }
    draw_finder(m, func, 0, 0);
    draw_finder(m, func, 0, size - 7);
    draw_finder(m, func, size - 7, 0);
    /* timing patterns */
    for (int i = 8; i <= size - 9; i++) {
        int bit = (i % 2 == 0) ? 1 : 0;
        m[6][i] = (uint8_t)bit; set_func(func, 6, i);
        m[i][6] = (uint8_t)bit; set_func(func, i, 6);
    }
    /* alignment patterns */
    for (int a = 0; a < v->nalign; a++) for (int b = 0; b < v->nalign; b++) {
        int ar = v->align[a], ac = v->align[b];
        /* skip the three finder corners */
        if ((ar <= 8 && ac <= 8) || (ar <= 8 && ac >= size - 9) || (ar >= size - 9 && ac <= 8)) continue;
        for (int r = -2; r <= 2; r++) for (int c = -2; c <= 2; c++) {
            int dark = (r == -2 || r == 2 || c == -2 || c == 2 || (r == 0 && c == 0));
            m[ar + r][ac + c] = (uint8_t)dark;
            set_func(func, ar + r, ac + c);
        }
    }
    /* dark module */
    m[size - 8][8] = 1; set_func(func, size - 8, 8);
    /* reserve format areas */
    for (int i = 0; i <= 8; i++) { set_func(func, 8, i); set_func(func, i, 8); }
    for (int i = 0; i < 8; i++) { set_func(func, 8, size - 1 - i); set_func(func, size - 1 - i, 8); }
}

static void place_data(JpqQr *qr, const QrVer *v, const uint8_t *bits, int nbits, const uint8_t func[JPQ_QR_MAX][JPQ_QR_MAX]) {
    int size = v->size, bit = 0, up = 1;
    for (int col = size - 1; col > 0; col -= 2) {
        if (col == 6) col--;
        for (int i = 0; i < size; i++) {
            int row = up ? size - 1 - i : i;
            for (int c = 0; c < 2; c++) {
                int cc = col - c;
                if (func[row][cc]) continue;
                int val = 0;
                if (bit < nbits) val = (bits[bit >> 3] >> (7 - (bit & 7))) & 1;
                bit++;
                qr->module[row][cc] = (uint8_t)val;
            }
        }
        up = !up;
    }
}

static int mask_bit(int mask, int r, int c) {
    switch (mask) {
    case 0: return (r + c) % 2 == 0;
    case 1: return r % 2 == 0;
    case 2: return c % 3 == 0;
    case 3: return (r + c) % 3 == 0;
    case 4: return (r / 2 + c / 3) % 2 == 0;
    case 5: return (r * c) % 2 + (r * c) % 3 == 0;
    case 6: return ((r * c) % 2 + (r * c) % 3) % 2 == 0;
    default: return ((r + c) % 2 + (r * c) % 3) % 2 == 0;
    }
}

static void apply_mask(JpqQr *qr, int mask, const uint8_t func[JPQ_QR_MAX][JPQ_QR_MAX]) {
    for (int r = 0; r < qr->size; r++) for (int c = 0; c < qr->size; c++)
        if (!func[r][c] && mask_bit(mask, r, c)) qr->module[r][c] ^= 1;
}

static void put_format(JpqQr *qr, int mask) {
    int size = qr->size;
    /* ECC level L = 01; 5 data bits (2 ecc + 3 mask). */
    int data = (1 << 3) | mask; /* 01 000..111 */
    int rem = data << 10;
    for (int i = 14; i >= 10; i--) if (rem & (1 << i)) rem ^= 0x537 << (i - 10);
    int fmt = ((data << 10) | (rem & 0x3FF)) ^ 0x5412;
    /* module[row][col]; bit i = (fmt >> i) & 1 (LSB first). First copy. */
    for (int i = 0; i <= 5; i++) qr->module[i][8] = (uint8_t)((fmt >> i) & 1);
    qr->module[7][8] = (uint8_t)((fmt >> 6) & 1);
    qr->module[8][8] = (uint8_t)((fmt >> 7) & 1);
    qr->module[8][7] = (uint8_t)((fmt >> 8) & 1);
    for (int i = 9; i < 15; i++) qr->module[8][14 - i] = (uint8_t)((fmt >> i) & 1);
    /* Second copy: horizontal on row 8, then vertical on column 8. */
    for (int i = 0; i < 8; i++) qr->module[8][size - 1 - i] = (uint8_t)((fmt >> i) & 1);
    for (int i = 8; i < 15; i++) qr->module[size - 15 + i][8] = (uint8_t)((fmt >> i) & 1);
    qr->module[size - 8][8] = 1; /* dark module */
}

/* Decide which of the eight masks draws the fewest penalties. */
static int penalty(const JpqQr *qr) {
    int size = qr->size, score = 0;
    /* Rule 1: runs of five or more same-colour modules in a row/column. */
    for (int r = 0; r < size; r++) {
        int run = 1;
        for (int c = 1; c < size; c++) {
            if (qr->module[r][c] == qr->module[r][c - 1]) run++;
            else { if (run >= 5) score += run - 2; run = 1; }
        }
        if (run >= 5) score += run - 2;
    }
    for (int c = 0; c < size; c++) {
        int run = 1;
        for (int r = 1; r < size; r++) {
            if (qr->module[r][c] == qr->module[r - 1][c]) run++;
            else { if (run >= 5) score += run - 2; run = 1; }
        }
        if (run >= 5) score += run - 2;
    }
    /* Rule 2: 2x2 blocks of one colour. */
    for (int r = 0; r < size - 1; r++) for (int c = 0; c < size - 1; c++) {
        int v = qr->module[r][c];
        if (v == qr->module[r][c + 1] && v == qr->module[r + 1][c] && v == qr->module[r + 1][c + 1]) score += 3;
    }
    /* Rule 3: finder-like 10111010000 / 00001011101 patterns. */
    for (int r = 0; r < size; r++) for (int c = 0; c < size; c++) {
        if (c + 10 < size) {
            int p[11]; for (int k = 0; k < 11; k++) p[k] = qr->module[r][c + k];
            int m1 = p[0] && !p[1] && p[2] && p[3] && p[4] && !p[5] && p[6] && !p[7] && !p[8] && !p[9] && !p[10];
            int m2 = !p[0] && !p[1] && !p[2] && !p[3] && p[4] && !p[5] && p[6] && p[7] && p[8] && !p[9] && p[10];
            if (m1 || m2) score += 40;
        }
        if (r + 10 < size) {
            int p[11]; for (int k = 0; k < 11; k++) p[k] = qr->module[r + k][c];
            int m1 = p[0] && !p[1] && p[2] && p[3] && p[4] && !p[5] && p[6] && !p[7] && !p[8] && !p[9] && !p[10];
            int m2 = !p[0] && !p[1] && !p[2] && !p[3] && p[4] && !p[5] && p[6] && p[7] && p[8] && !p[9] && p[10];
            if (m1 || m2) score += 40;
        }
    }
    /* Rule 4: proportion of dark modules. */
    int dark = 0, total = size * size;
    for (int r = 0; r < size; r++) for (int c = 0; c < size; c++) dark += qr->module[r][c];
    int pct = dark * 100 / total;
    int prev = (pct / 5) * 5, next = prev + 5;
    int d = prev - 50; if (d < 0) d = -d;
    int e = next - 50; if (e < 0) e = -e;
    score += (d < e ? d : e) * 10;
    return score;
}

int jpq_qr_encode(const char *text, int force_mask, JpqQr *out) {
    gf_init();
    size_t len = 0;
    while (text && text[len]) len++;
    if (!out || len > 78) return -1;
    const QrVer *v;
    if (choose_version(len, &v)) return -1;
    out->version = v->version;
    out->size = v->size;
    out->mask = 0;

    /* Bit stream: mode 0100, 8-bit count, bytes, terminator, pad. */
    uint8_t bits[1024];
    for (int i = 0; i < 1024; i++) bits[i] = 0;
    int nbits = 0;
    #define PUTB(value, n) do { for (int _i = (n) - 1; _i >= 0; _i--) { if ((value) & (1 << _i)) bits[nbits >> 3] |= (uint8_t)(1 << (7 - (nbits & 7))); nbits++; } } while (0)
    PUTB(4, 4);
    PUTB((int)len, 8);
    for (size_t i = 0; i < len; i++) PUTB((unsigned char)text[i], 8);
    int cap_bits = v->data * 8;
    int term = cap_bits - nbits; if (term > 4) term = 4;
    PUTB(0, term);
    while (nbits % 8) PUTB(0, 1);
    int pad = 0xEC;
    while (nbits < cap_bits) { PUTB(pad, 8); pad = pad == 0xEC ? 0x11 : 0xEC; }
    #undef PUTB

    uint8_t codewords[120];
    for (int i = 0; i < v->data; i++) codewords[i] = bits[i];
    uint8_t ecc[64];
    rs_encode(codewords, v->data, v->ecc, ecc);
    uint8_t all[160];
    int total = v->data + v->ecc;
    for (int i = 0; i < v->data; i++) all[i] = codewords[i];
    for (int i = 0; i < v->ecc; i++) all[v->data + i] = ecc[i];

    uint8_t func[JPQ_QR_MAX][JPQ_QR_MAX];
    setup_function(v, out->module, func);
    place_data(out, v, all, total * 8, func);

    int mask = force_mask;
    if (mask < 0) {
        int best = 0, best_score = -1;
        for (int m = 0; m < 8; m++) {
            apply_mask(out, m, func);
            put_format(out, m);
            int s = penalty(out);
            if (best_score < 0 || s < best_score) { best_score = s; best = m; }
            apply_mask(out, m, func); /* undo */
        }
        mask = best;
    }
    apply_mask(out, mask, func);
    put_format(out, mask);
    out->mask = mask;
    return 0;
}
