/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <string.h>
#include <tetra/tetra_coding.h>
#include "lmac.h"

const struct lmac_chan_def lmac_chan_defs[3] = {
    [LMAC_SCH_S] = { 60, 120, 11 },
    [LMAC_SCH_H] = { 124, 216, 101 },
    [LMAC_SCH_F] = { 268, 432, 103 },
};

/* Scrambling sequence for init, from a two-entry cache. */
static const uint32_t *scr_seq(struct tetra_lmac_ws *ws, uint32_t init)
{
    unsigned e;

    if (ws->scr_init[0] == init)
        return ws->scr_seq[0];
    if (ws->scr_init[1] == init)
        return ws->scr_seq[1];
    /* Valid states have bits 30 and 31 set: a zeroed cache never hits. */
    e = ws->scr_next;
    ws->scr_next ^= 1;
    ws->scr_init[e] = init;
    tetra_scramb_words(init, ws->scr_seq[e], TETRA_SCR_WORDS);
    return ws->scr_seq[e];
}

void lmac_descramble(struct tetra_lmac_ws *ws, int8_t *soft, unsigned n,
                     uint32_t scramb)
{
    const uint32_t *q = scr_seq(ws, scramb);

    for (unsigned i = 0; i < n; i++) {
        if (q[i >> 5] & (0x80000000u >> (i & 31)))
            soft[i] = (int8_t)(soft[i] == -128 ? 127 : -soft[i]);
    }
}

/* CRC-16 of tetra_crc16() over k1 packed bits, four bits per step. */
static uint16_t crc16_packed(const uint8_t *buf, unsigned k1)
{
    static const uint16_t nib[16] = {
        0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
        0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    };
    uint16_t crc = 0xffff;
    unsigned i = 0;

    for (; i + 4 <= k1; i += 4) {
        unsigned v = (buf[i >> 3] >> (4 - (i & 4))) & 0xf;

        crc = (uint16_t)((crc << 4) ^ nib[(crc >> 12) ^ v]);
    }
    for (; i < k1; i++) {
        unsigned bit = (buf[i >> 3] >> (7 - (i & 7))) & 1;
        unsigned fb = (crc >> 15) ^ bit;

        crc = (uint16_t)((crc << 1) ^ (fb ? 0x1021 : 0));
    }
    return (uint16_t)~crc;
}

bool lmac_decode(struct tetra_lmac_ws *ws, enum lmac_chan ch,
                 const int8_t *type5, uint32_t scramb, uint8_t *out)
{
    const struct lmac_chan_def *d = &lmac_chan_defs[ch];
    unsigned k2 = d->k1 + 16 + 4;

    memcpy(ws->t5, type5, d->k5);
    lmac_descramble(ws, ws->t5, d->k5, scramb);
    tetra_deinterleave_soft(ws->t5, ws->t3, d->k5, d->a);
    tetra_depuncture(TETRA_PUNCT_2_3, ws->t3, d->k5, ws->mother, k2);
    if (tetra_viterbi_buf(ws->mother, k2, ws->t2, ws->dec))
        return false;
    tetra_pack_bits(ws->t2, d->k1, out);

    uint16_t rx_crc = 0;

    for (unsigned i = 0; i < 16; i++)
        rx_crc = (uint16_t)((rx_crc << 1) | ws->t2[d->k1 + i]);
    return crc16_packed(out, d->k1) == rx_crc;
}

void lmac_encode(enum lmac_chan ch, const uint8_t *type1, uint32_t scramb,
                 uint8_t *type5)
{
    const struct lmac_chan_def *d = &lmac_chan_defs[ch];
    unsigned k2 = d->k1 + 16 + 4, j = 0, s = 0;
    uint8_t t2[292];
    uint8_t t3[432];

    memcpy(t2, type1, d->k1);
    tetra_crc16_append(t2, d->k1);
    memset(&t2[d->k1 + 16], 0, 4);

    /*
     * Rate 2/3 puncturing keeps mother code bits 1, 2 and 5 of every 8:
     * V1 and V2 of even type-2 bits, V1 of odd ones (clause 8.2.3.1.3).
     * Producing them directly avoids storing the mother codeword.
     */
    for (unsigned k = 0; k < k2; k++) {
        unsigned r = ((unsigned)t2[k] << 4) | s;
        uint8_t o = conv_out_nibble(r);

        t3[j++] = (o >> 3) & 1;
        if (!(k & 1))
            t3[j++] = (o >> 2) & 1;
        s = r >> 1;
    }
    tetra_interleave(t3, type5, d->k5, d->a);
    tetra_scramble(type5, d->k5, scramb);
}
