/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * Burst assembly, EN 300 392-2 clause 9.4.4 and EN 300 396-2 clause 9.4.
 */

#include <string.h>
#include <tetra/tetra_coding.h>
#include <tetra/tetra_tx.h>
#include "l1.h"
#include "l2/lmac.h"

void tetra_encode_sch(enum tetra_sch ch, const uint8_t *type1, uint32_t scramb,
                      uint8_t *type5)
{
    lmac_encode((enum lmac_chan)ch, type1, scramb, type5);
}

void tetra_encode_aach(uint16_t data, uint32_t scramb, uint8_t bb[30])
{
    uint32_t cw = tetra_rm3014_encode(data);

    for (unsigned i = 0; i < 30; i++)
        bb[i] = (cw >> (29 - i)) & 1;
    tetra_scramble(bb, 30, scramb);
}

/* Phase transition of symbol (b1, b2) in units of pi/4, table 1. */
static int dphi(uint8_t b1, uint8_t b2)
{
    static const int tab[4] = { 1, 3, -1, -3 }; /* 00 01 10 11 */

    return tab[(b1 << 1) | b2];
}

/*
 * Set the phase adjustment bits at bit index pos (0-based) so that they
 * cancel the phase rotation of symbols n1..n2 (1-based) of the burst.
 */
static void phase_adjust(uint8_t *burst, unsigned pos, unsigned n1, unsigned n2)
{
    static const uint8_t sym[8][2] = {
        { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 1 },
        { 0, 0 }, { 1, 1 }, { 0, 0 }, { 1, 0 },
    };
    int sum = 0;
    unsigned need;

    for (unsigned n = n1; n <= n2; n++)
        sum += dphi(burst[2 * n - 2], burst[2 * n - 1]);
    need = (unsigned)((-sum) & 7); /* odd: 1, 3, 5 (-3) or 7 (-1) */
    burst[pos] = sym[need][0];
    burst[pos + 1] = sym[need][1];
}

void tetra_dmo_build_dsb(uint8_t burst[470], const uint8_t sb[120],
                         const uint8_t bkn2[216])
{
    memcpy(burst, tetra_dmo_p3, 12);
    memcpy(burst + 14, tetra_seq_f, 80);
    memcpy(burst + 94, sb, 120);
    memcpy(burst + 214, tetra_seq_y, 38);
    memcpy(burst + 252, bkn2, 216);
    burst[468] = 0;
    burst[469] = 0;
    phase_adjust(burst, 12, 8, 126);
}

void tetra_dmo_build_dnb(uint8_t burst[470], const uint8_t bkn1[216],
                         const uint8_t bkn2[216], bool stch)
{
    memcpy(burst, stch ? tetra_dmo_p2 : tetra_dmo_p1, 12);
    memcpy(burst + 14, bkn1, 216);
    memcpy(burst + 230, stch ? tetra_seq_p : tetra_seq_n, 22);
    memcpy(burst + 252, bkn2, 216);
    burst[468] = 0;
    burst[469] = 0;
    phase_adjust(burst, 12, 8, 126);
}

void tetra_tmo_build_ndb(uint8_t slot[510], const uint8_t bkn1[216],
                         const uint8_t bb[30], const uint8_t bkn2[216],
                         bool two_blocks)
{
    memcpy(slot, tetra_seq_q + 10, 12);
    memcpy(slot + 14, bkn1, 216);
    memcpy(slot + 230, bb, 14);
    memcpy(slot + 244, two_blocks ? tetra_seq_p : tetra_seq_n, 22);
    memcpy(slot + 266, bb + 14, 16);
    memcpy(slot + 282, bkn2, 216);
    memcpy(slot + 500, tetra_seq_q, 10);
    phase_adjust(slot, 12, 8, 122);
    phase_adjust(slot, 498, 123, 249);
}

void tetra_tmo_build_sb(uint8_t slot[510], const uint8_t sb[120],
                        const uint8_t bb[30], const uint8_t bkn2[216])
{
    memcpy(slot, tetra_seq_q + 10, 12);
    memcpy(slot + 14, tetra_seq_f, 80);
    memcpy(slot + 94, sb, 120);
    memcpy(slot + 214, tetra_seq_y, 38);
    memcpy(slot + 252, bb, 30);
    memcpy(slot + 282, bkn2, 216);
    memcpy(slot + 500, tetra_seq_q, 10);
    phase_adjust(slot, 12, 8, 108);
    phase_adjust(slot, 498, 109, 249);
}
