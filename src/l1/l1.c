/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <tetra/tetra_coding.h>
#include "l1.h"

/* EN 300 392-2 (9.4) .. (9.6), EN 300 396-2 (63), (64) */
const uint8_t tetra_seq_n[22] = { 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1,
                                  0, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0 };
const uint8_t tetra_seq_p[22] = { 0, 1, 1, 1, 1, 0, 1, 0, 0, 1, 0,
                                  0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0 };
const uint8_t tetra_seq_q[22] = { 1, 0, 1, 1, 0, 1, 1, 1, 0, 0, 0,
                                  0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1 };

/* EN 300 392-2 (9.11), EN 300 396-2 (66) */
const uint8_t tetra_seq_y[38] = { 1, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1,
                                  1, 0, 0, 1, 1, 1, 0, 1, 0, 0, 1, 1, 1,
                                  0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1 };

/* EN 300 392-2 (9.1) .. (9.3): 8 ones, 64 zeros, 8 ones */
const uint8_t tetra_seq_f[80] = {
    1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1,
};

/* EN 300 396-2 (61), (62), (65) */
const uint8_t tetra_dmo_p1[12] = { 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1 };
const uint8_t tetra_dmo_p2[12] = { 1, 0, 0, 1, 1, 0, 1, 0, 1, 0, 0, 1 };
const uint8_t tetra_dmo_p3[12] = { 0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 1, 1 };

void rxcore_copy(const struct tetra_rxcore *c, uint32_t idx, int8_t *out,
                 unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        out[i] = rxcore_at(c, idx + i);
}

int rxcore_corr(const struct tetra_rxcore *c, uint32_t idx, const uint8_t *seq,
                unsigned n)
{
    int32_t acc = 0, mag = 0;

    for (unsigned i = 0; i < n; i++) {
        int s = rxcore_at(c, idx + i);

        acc += seq[i] ? s : -s;
        mag += s < 0 ? -s : s;
    }
    if (mag == 0)
        return 0;
    return (int)(acc * 1000 / mag);
}

void rxcore_push_dibit(struct tetra_rxcore *c, uint8_t d)
{
    rxcore_push(c, (d & 2) ? TETRA_SOFT_ONE : TETRA_SOFT_ZERO);
    rxcore_push(c, (d & 1) ? TETRA_SOFT_ONE : TETRA_SOFT_ZERO);
}
