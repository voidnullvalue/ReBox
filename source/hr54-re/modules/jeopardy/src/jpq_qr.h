#ifndef JPQ_QR_H
#define JPQ_QR_H
/* Compact QR encoder (byte mode, ECC level L, versions 1..4, single RS block).
 *
 * Written for this module from the ISO/IEC 18004 model; no third-party code is
 * vendored so there is no extra licence to track. URLs used on the lobby screen
 * are short enough for version 4-L (78 bytes). A self-check decoder lives in the
 * host tests, and the test suite cross-checks the matrix against the host
 * `qrcode` reference implementation. */
#include <stddef.h>
#include <stdint.h>

#define JPQ_QR_MAX 33

typedef struct {
    int size;
    int version;
    int mask;
    uint8_t module[JPQ_QR_MAX][JPQ_QR_MAX]; /* 1 = dark */
} JpqQr;

/* Encode text (NUL-terminated, <=78 bytes). force_mask -1 selects the best of
 * the 8 masks by the standard penalty; 0..7 forces a mask (used by tests to
 * match the reference encoder). Returns 0 on success. */
int jpq_qr_encode(const char *text, int force_mask, JpqQr *out);
#endif
