/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * Channel coding for speech, EN 300 395-2 clauses 5 and 6:
 * sensitivity classes, CRC, RCPC with a rate 1/3 mother code, interleaving.
 */

#include <string.h>
#include <tetra/tetra_coding.h>
#include <tetra/tetra_speech.h>
#include <tetra/tetra_tx.h>
#include "lmac.h"

/*
 * Speech frame bit (B1..B137) at each position of the type-2 block, from
 * table 6 (table 5 alternates frames A and B in the same order): 51 class 0
 * bits, 56 class 1 bits, then 30 class 2 bits.
 */
static const uint8_t speech_order[TETRA_SPEECH_BITS] = {
    35,  36,  37,  38,  39,  40,  41,  42,  43,  47,  48,  56,  61,  62,
    63,  64,  65,  66,  67,  68,  69,  70,  74,  75,  83,  88,  89,  90,
    91,  92,  93,  94,  95,  96,  97,  101, 102, 110, 115, 116, 117, 118,
    119, 120, 121, 122, 123, 124, 128, 129, 137, 58,  85,  112, 54,  81,
    108, 135, 50,  77,  104, 131, 45,  72,  99,  126, 55,  82,  109, 136,
    5,   13,  34,  8,   16,  17,  22,  23,  24,  25,  26,  6,   14,  7,
    15,  60,  87,  114, 46,  73,  100, 127, 44,  71,  98,  125, 33,  49,
    76,  103, 130, 59,  86,  113, 57,  84,  111, 18,  19,  20,  21,  31,
    32,  53,  80,  107, 134, 1,   2,   3,   4,   9,   10,  11,  12,  27,
    28,  29,  30,  52,  79,  106, 133, 51,  78,  105, 132,
};

#define CLASS0_BITS 51
#define CLASS1_BITS 56
#define CLASS2_BITS 30

/*
 * Rate 1/3 mother code (clause 5.4.3.1): output bits (V1 in bit 2) for each
 * register value, generators 1+D+D2+D3+D4, 1+D+D3+D4 and 1+D2+D4.
 */
static const uint8_t out3[32] = {
    0x0, 0x7, 0x6, 0x1, 0x5, 0x2, 0x3, 0x4, 0x6, 0x1, 0x0,
    0x7, 0x3, 0x4, 0x5, 0x2, 0x7, 0x0, 0x1, 0x6, 0x2, 0x5,
    0x4, 0x3, 0x1, 0x6, 0x7, 0x0, 0x4, 0x3, 0x2, 0x5,
};

struct punct {
    const uint8_t *p;
    uint8_t t;
    uint8_t period;
};

/* Clauses 5.5.2.1, 5.5.2.2 and 5.6.2.1 */
static const uint8_t p_2_3[3] = { 1, 2, 4 };
static const uint8_t p_8_18[9] = { 1, 2, 3, 4, 5, 7, 8, 10, 11 };
static const uint8_t p_8_17[17] = { 1,  2,  3,  4,  5,  7,  8,  10, 11,
                                    13, 14, 16, 17, 19, 20, 22, 23 };

static const struct punct punct_class1 = { p_2_3, 3, 6 };
static const struct punct punct_full2 = { p_8_18, 9, 12 };
static const struct punct punct_half2 = { p_8_17, 17, 24 };

/* Block layout for a full slot (clause 5.5) and a half slot (5.6). */
struct layout {
    uint16_t frames;   /* speech frames coded together */
    uint16_t k_class0; /* class 0 type-2 bits */
    uint16_t k_class1; /* class 1 type-2 bits */
    uint16_t k_class2; /* class 2 + parity + tail type-2 bits */
    uint16_t n3_class1;
    uint16_t n3_class2;
    uint8_t parity;   /* CRC parity bits */
    uint8_t crc_poly; /* G(X) without X^parity, X^0 in bit 0 */
    bool overall;     /* extra overall parity bit (full slot) */
    const struct punct *punct2;
};

static const struct layout full = {
    2,
    2 * CLASS0_BITS,
    2 * CLASS1_BITS,
    2 * CLASS2_BITS + 8 + 4,
    168,
    162,
    7,
    0x09,
    true,
    &punct_full2,
};

static const struct layout half = {
    1, CLASS0_BITS, CLASS1_BITS, CLASS2_BITS + 4 + 4, 84, 81,
    4, 0x03,        false,       &punct_half2,
};

/* Mother code index (0-based) of type-3 bit j (1-based), formula (49). */
static unsigned punct_index(const struct punct *p, unsigned j)
{
    unsigned q = (j - 1) / p->t;

    return p->period * q + p->p[j - 1 - p->t * q] - 1u;
}

/*
 * CRC parity bits of clause 5.4.2: F(X) = X^p I(X) mod G(X) with
 * I(X) = C1(1) + C1(2) X + ... Returns f(0) in bit 0.
 */
static unsigned speech_crc(const uint8_t *bits, unsigned k1, unsigned p,
                           unsigned poly)
{
    unsigned reg = 0, mask = (1u << p) - 1;

    for (unsigned i = k1; i-- > 0;) {
        unsigned fb = (bits[i] ^ (reg >> (p - 1))) & 1;

        reg = (reg << 1) & mask;
        if (fb)
            reg ^= poly;
    }
    return reg;
}

/* Number of class 2 information bits covered by the CRC. */
static unsigned crc_k1(const struct layout *l)
{
    return l->frames * CLASS2_BITS;
}

static void set_parity(const struct layout *l, uint8_t *t2)
{
    unsigned c2 = l->k_class0 + l->k_class1, k1 = crc_k1(l);
    unsigned f = speech_crc(&t2[c2], k1, l->parity, l->crc_poly);
    unsigned sum = 0;

    for (unsigned i = 0; i < l->parity; i++) {
        t2[c2 + k1 + i] = (f >> i) & 1;
        sum ^= t2[c2 + k1 + i];
    }
    if (l->overall) {
        for (unsigned i = 0; i < k1; i++)
            sum ^= t2[c2 + i];
        t2[c2 + k1 + l->parity] = (uint8_t)sum;
    }
}

static bool check_parity(const struct layout *l, const uint8_t *t2)
{
    unsigned c2 = l->k_class0 + l->k_class1, k1 = crc_k1(l);
    unsigned f = speech_crc(&t2[c2], k1, l->parity, l->crc_poly);
    unsigned sum = 0;

    for (unsigned i = 0; i < l->parity; i++) {
        if (t2[c2 + k1 + i] != ((f >> i) & 1))
            return false;
        sum ^= (f >> i) & 1;
    }
    if (l->overall) {
        for (unsigned i = 0; i < k1; i++)
            sum ^= t2[c2 + i];
        if (t2[c2 + k1 + l->parity] != sum)
            return false;
    }
    return true;
}

/* Type-2 position of bit index i (0..136) of frame f. */
static unsigned t2_pos(const struct layout *l, unsigned f, unsigned i)
{
    return l->frames * i + f;
}

static void frames_to_t2(const struct layout *l, const uint8_t *a,
                         const uint8_t *b, uint8_t *t2)
{
    unsigned n = l->k_class0 + l->k_class1 + l->k_class2;

    memset(t2, 0, n);
    for (unsigned i = 0; i < TETRA_SPEECH_BITS; i++) {
        t2[t2_pos(l, 0, i)] = a[speech_order[i] - 1] & 1;
        if (l->frames == 2)
            t2[t2_pos(l, 1, i)] = b[speech_order[i] - 1] & 1;
    }
    set_parity(l, t2);
}

static void t2_to_frames(const struct layout *l, const uint8_t *t2, uint8_t *a,
                         uint8_t *b)
{
    for (unsigned i = 0; i < TETRA_SPEECH_BITS; i++) {
        a[speech_order[i] - 1] = t2[t2_pos(l, 0, i)];
        if (l->frames == 2)
            b[speech_order[i] - 1] = t2[t2_pos(l, 1, i)];
    }
}

/* Encode type-2 bits into type-3 bits (classes 1 and 2 convolved). */
static void encode_t3(const struct layout *l, const uint8_t *t2, uint8_t *t3)
{
    unsigned k2 = l->k_class1 + l->k_class2, s = 0;
    const uint8_t *in = &t2[l->k_class0];
    uint8_t mother[3 * 184];

    memcpy(t3, t2, l->k_class0);
    for (unsigned k = 0; k < k2; k++) {
        unsigned r = ((unsigned)in[k] << 4) | s;
        uint8_t o = out3[r];

        mother[3 * k] = (o >> 2) & 1;
        mother[3 * k + 1] = (o >> 1) & 1;
        mother[3 * k + 2] = o & 1;
        s = r >> 1;
    }

    uint8_t *c1 = &t3[l->k_class0];
    uint8_t *c2 = c1 + l->n3_class1;
    const uint8_t *m2 = &mother[3 * l->k_class1];

    for (unsigned j = 1; j <= l->n3_class1; j++)
        c1[j - 1] = mother[punct_index(&punct_class1, j)];
    for (unsigned j = 1; j <= l->n3_class2; j++)
        c2[j - 1] = m2[punct_index(l->punct2, j)];
}

/* Decode type-3 soft bits into type-2 bits. */
static void decode_t3(const struct layout *l, struct tetra_speech_ws *ws)
{
    unsigned k2 = l->k_class1 + l->k_class2;
    const int8_t *c1 = &ws->t3[l->k_class0];
    const int8_t *c2 = c1 + l->n3_class1;
    int8_t *m2 = &ws->mother[3 * l->k_class1];

    for (unsigned i = 0; i < l->k_class0; i++)
        ws->t2[i] = ws->t3[i] > 0;

    memset(ws->mother, 0, 3 * k2);
    for (unsigned j = 1; j <= l->n3_class1; j++)
        ws->mother[punct_index(&punct_class1, j)] = c1[j - 1];
    for (unsigned j = 1; j <= l->n3_class2; j++)
        m2[punct_index(l->punct2, j)] = c2[j - 1];

    viterbi_core(ws->mother, 3, out3, k2, &ws->t2[l->k_class0], ws->dec);
}

/* Matrix interleaving of clause 5.5.3: C4(i * 24 + j) = C3(j * 18 + i). */
void tetra_speech_encode_slot(const uint8_t *a, const uint8_t *b,
                              uint8_t *type4)
{
    uint8_t t2[286], t3[432];

    frames_to_t2(&full, a, b, t2);
    encode_t3(&full, t2, t3);
    for (unsigned i = 0; i < 18; i++) {
        for (unsigned j = 0; j < 24; j++)
            type4[i * 24 + j] = t3[j * 18 + i];
    }
}

void tetra_speech_encode_half(const uint8_t *a, uint8_t *type4)
{
    uint8_t t2[145], t3[216];

    frames_to_t2(&half, a, NULL, t2);
    encode_t3(&half, t2, t3);
    tetra_interleave(t3, type4, 216, 101);
}

bool tetra_speech_decode_slot(struct tetra_speech_ws *ws, const int8_t *type4,
                              uint8_t *a, uint8_t *b)
{
    for (unsigned i = 0; i < 18; i++) {
        for (unsigned j = 0; j < 24; j++)
            ws->t3[j * 18 + i] = type4[i * 24 + j];
    }
    decode_t3(&full, ws);
    t2_to_frames(&full, ws->t2, a, b);
    return check_parity(&full, ws->t2);
}

bool tetra_speech_decode_half(struct tetra_speech_ws *ws, const int8_t *type4,
                              uint8_t *a)
{
    tetra_deinterleave_soft(type4, ws->t3, 216, 101);
    decode_t3(&half, ws);
    t2_to_frames(&half, ws->t2, a, NULL);
    return check_parity(&half, ws->t2);
}

int tetra_speech_decode(struct tetra_speech_ws *ws,
                        const struct tetra_traffic *t, struct tetra_speech *out)
{
    bool ok;

    memset(out, 0, sizeof(*out));
    out->bfi[0] = true;
    out->bfi[1] = true;
    if (t->circuit_type != 0 || t->half[1] != TETRA_HALF_TCH)
        return 0;

    if (t->half[0] == TETRA_HALF_TCH) {
        ok = tetra_speech_decode_slot(ws, t->bits, out->bits[0], out->bits[1]);
        out->bfi[0] = !ok;
        out->bfi[1] = !ok;
        return ok ? 2 : 0;
    }

    /* First half stolen: only the second frame is speech. */
    ok = tetra_speech_decode_half(ws, t->bits + 216, out->bits[1]);
    out->bfi[1] = !ok;
    return ok ? 1 : 0;
}
