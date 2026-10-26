/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * 16-state RCPC code: rate 1/4 mother code, puncturing and a soft-decision
 * Viterbi decoder. EN 300 392-2 clause 8.2.3.1, EN 300 396-2 clause 8.2.3.1.
 */

#include <string.h>
#include <tetra/tetra_coding.h>
#include "lmac.h"

/*
 * Register r = b(k) << 4 | b(k-1) << 3 | ... | b(k-4).
 * G1 = 1 + D + D^4, G2 = 1 + D^2 + D^3 + D^4,
 * G3 = 1 + D + D^2 + D^4, G4 = 1 + D + D^3 + D^4.
 */
/*
 * Encoder output nibble (V1 in bit 3 .. V4 in bit 0) for each register
 * value, derived from the generator masks 0x19, 0x17, 0x1d, 0x1b.
 */
static const uint8_t out_tab[32] = {
    0x0, 0xf, 0x5, 0xa, 0x6, 0x9, 0x3, 0xc, 0xb, 0x4, 0xe,
    0x1, 0xd, 0x2, 0x8, 0x7, 0xf, 0x0, 0xa, 0x5, 0x9, 0x6,
    0xc, 0x3, 0x4, 0xb, 0x1, 0xe, 0x2, 0xd, 0x7, 0x8,
};

static inline uint8_t conv_out(unsigned r)
{
    return out_tab[r & 0x1f];
}

uint8_t conv_out_nibble(unsigned r)
{
    return conv_out(r);
}

void tetra_conv_encode(const uint8_t *in, unsigned k2, uint8_t *mother)
{
    unsigned s = 0;

    for (unsigned k = 0; k < k2; k++) {
        unsigned r = ((unsigned)(in[k] & 1) << 4) | s;
        uint8_t o = conv_out(r);

        mother[4 * k + 0] = (o >> 3) & 1;
        mother[4 * k + 1] = (o >> 2) & 1;
        mother[4 * k + 2] = (o >> 1) & 1;
        mother[4 * k + 3] = o & 1;
        s = r >> 1;
    }
}

struct punct_def {
    uint8_t t;
    uint8_t p[6];
    uint8_t div; /* i = j + (j - 1) div div, 0 for i = j */
};

static const struct punct_def punct_defs[] = {
    [TETRA_PUNCT_2_3] = { 3, { 1, 2, 5 }, 0 },
    [TETRA_PUNCT_292_432] = { 3, { 1, 2, 5 }, 65 },
    [TETRA_PUNCT_148_432] = { 6, { 1, 2, 3, 5, 6, 7 }, 35 },
};

/* Mother code index (0-based) of type-3 bit j (1-based). */
static unsigned punct_index(const struct punct_def *d, unsigned j)
{
    unsigned i = j;
    unsigned q;

    if (d->div)
        i += (j - 1) / d->div;
    q = (i - 1) / d->t;
    return 8 * q + d->p[i - d->t * q - 1] - 1;
}

void tetra_puncture(enum tetra_punct p, const uint8_t *mother, uint8_t *out,
                    unsigned k3)
{
    const struct punct_def *d = &punct_defs[p];

    for (unsigned j = 1; j <= k3; j++)
        out[j - 1] = mother[punct_index(d, j)];
}

void tetra_depuncture(enum tetra_punct p, const int8_t *in, unsigned k3,
                      int8_t *mother, unsigned k2)
{
    const struct punct_def *d = &punct_defs[p];

    memset(mother, 0, 4 * k2);
    for (unsigned j = 1; j <= k3; j++) {
        unsigned idx = punct_index(d, j);

        if (idx < 4 * k2)
            mother[idx] = in[j - 1];
    }
}

int tetra_viterbi(const int8_t *mother, unsigned k2, uint8_t *out)
{
    uint16_t dec[TETRA_VITERBI_MAX_K2];

    return tetra_viterbi_buf(mother, k2, out, dec);
}

int tetra_viterbi_buf(const int8_t *mother, unsigned k2, uint8_t *out,
                      uint16_t *dec)
{
    if (k2 > TETRA_VITERBI_MAX_K2)
        return -1;
    viterbi_core(mother, 4, out_tab, k2, out, dec);
    return 0;
}

/*
 * Every generator taps both ends of the register, so the two branches
 * leaving a state and the two entering one carry complementary outputs:
 * the trellis splits into 8 butterflies with one branch metric each.
 * Path metrics grow by at most nout * 128 per step, so int32 metrics need
 * no normalisation for blocks up to TETRA_VITERBI_MAX_K2 steps.
 */
void viterbi_core(const int8_t *mother, unsigned nout, const uint8_t *tab,
                  unsigned k2, uint8_t *out, uint16_t *dec)
{
    int32_t buf[2][16];
    int32_t *pm = buf[0], *npm = buf[1];

    for (unsigned s = 0; s < 16; s++)
        pm[s] = (s == 0) ? 0 : -(1 << 24);

    for (unsigned k = 0; k < k2; k++) {
        const int8_t *v = &mother[nout * k];
        int32_t hi[4], lo[4];
        uint32_t d = 0;

        /* Correlation metric of output o is hi[o >> 2] + lo[o & 3]. */
        if (nout == 4) {
            hi[3] = v[0] + v[1];
            hi[2] = v[0] - v[1];
        } else {
            hi[3] = v[0];
            hi[2] = -v[0];
        }
        hi[1] = -hi[2];
        hi[0] = -hi[3];
        lo[3] = v[nout - 2] + v[nout - 1];
        lo[2] = v[nout - 2] - v[nout - 1];
        lo[1] = -lo[2];
        lo[0] = -lo[3];

        /* States j and j + 8 both come from 2j and 2j + 1. */
        for (unsigned j = 0; j < 8; j++) {
            unsigned o = tab[2 * j];
            int32_t m = hi[o >> 2] + lo[o & 3];
            int32_t a = pm[2 * j], b = pm[2 * j + 1];

            if (b - m > a + m) {
                npm[j] = b - m;
                d |= 1u << j;
            } else {
                npm[j] = a + m;
            }
            if (b + m > a - m) {
                npm[j + 8] = b + m;
                d |= 0x100u << j;
            } else {
                npm[j + 8] = a - m;
            }
        }
        dec[k] = (uint16_t)d;

        int32_t *t = pm;

        pm = npm;
        npm = t;
    }

    /* Trace back from state 0 (tail bits). */
    unsigned n = 0;

    for (unsigned k = k2; k-- > 0;) {
        unsigned bit = (dec[k] >> n) & 1;

        out[k] = (uint8_t)(n >> 3);
        n = ((n << 1) & 0xf) | bit;
    }
}
