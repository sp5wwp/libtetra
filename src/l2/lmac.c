/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <string.h>
#include <tetra/tetra_coding.h>
#include "lmac.h"

const struct lmac_chan_def lmac_chan_defs[3] = {
    [LMAC_SCH_S] = { 60, 120, 11 },
    [LMAC_SCH_H] = { 124, 216, 101 },
    [LMAC_SCH_F] = { 268, 432, 103 },
};

bool lmac_decode(struct tetra_lmac_ws *ws, enum lmac_chan ch,
                 const int8_t *type5, uint32_t scramb, uint8_t *out)
{
    const struct lmac_chan_def *d = &lmac_chan_defs[ch];
    unsigned k2 = d->k1 + 16 + 4;

    memcpy(ws->t5, type5, d->k5);
    tetra_descramble_soft(ws->t5, d->k5, scramb);
    tetra_deinterleave_soft(ws->t5, ws->t3, d->k5, d->a);
    tetra_depuncture(TETRA_PUNCT_2_3, ws->t3, d->k5, ws->mother, k2);
    if (tetra_viterbi_buf(ws->mother, k2, ws->t2, ws->dec))
        return false;
    tetra_pack_bits(ws->t2, d->k1, out);
    return tetra_crc16_check(ws->t2, d->k1);
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

void lmac_tch_descramble(int8_t *soft, unsigned n, uint32_t scramb)
{
    tetra_descramble_soft(soft, n, scramb);
}
