/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_L1_H
#define TETRA_L1_H

/*
 * Layer 1: bit history, training sequences and burst field positions.
 * EN 300 392-2 clause 9.4.4, EN 300 396-2 clause 9.4.
 */

#include <stdbool.h>
#include <stdint.h>
#include <tetra/tetra_common.h>

#define SLOT_BITS 510

/* Training sequences, EN 300 392-2 9.4.4.3 and EN 300 396-2 9.4.3.3. */
extern const uint8_t tetra_seq_n[22]; /* normal training sequence 1 */
extern const uint8_t tetra_seq_p[22]; /* normal training sequence 2 */
extern const uint8_t tetra_seq_y[38]; /* synchronisation training seq */
extern const uint8_t tetra_seq_f[80]; /* frequency correction field */
extern const uint8_t tetra_seq_q[22]; /* normal training sequence 3 */
extern const uint8_t tetra_dmo_p1[12];
extern const uint8_t tetra_dmo_p2[12];
extern const uint8_t tetra_dmo_p3[12];

/* The 38-bit sync sequence as an integer, y1 in bit 37. */
#define TETRA_SEQ_Y_WORD 0x30673a7067ull
#define TETRA_SEQ_Y_MASK ((1ull << 38) - 1)

/*
 * Bit offsets (0-based) inside a 510-bit TMO downlink slot,
 * continuous bursts (table 9.7 and 9.9).
 */
#define TMO_NDB_BLK1 14
#define TMO_NDB_BB1 230 /* 14 bits */
#define TMO_NDB_TRAIN 244
#define TMO_NDB_BB2 266 /* 16 bits */
#define TMO_NDB_BLK2 282
#define TMO_SB_SB 94
#define TMO_SB_TRAIN 214
#define TMO_SB_BB 252
#define TMO_SB_BLK2 282

/*
 * Bit offsets (0-based) inside a 510-bit DMO slot. The burst starts after
 * 34 bits of guard / ramping (burst delay d = 17 symbols).
 */
#define DMO_BURST_START 34
#define DMO_DNB_BLK1 (DMO_BURST_START + 14)
#define DMO_DNB_TRAIN (DMO_BURST_START + 230)
#define DMO_DNB_BLK2 (DMO_BURST_START + 252)
#define DMO_DSB_SB (DMO_BURST_START + 94)
#define DMO_DSB_TRAIN (DMO_BURST_START + 214)
#define DMO_DSB_BLK2 (DMO_BURST_START + 252)

/** Push one soft bit into the history. */
static inline void rxcore_push(struct tetra_rxcore *c, int8_t s)
{
    c->ring[c->wr & (TETRA_RING_BITS - 1)] = s;
    c->hard_sr = (c->hard_sr << 1) | (uint64_t)(s > 0);
    c->wr++;
}

/** Soft bit at absolute index (must still be in the history). */
static inline int8_t rxcore_at(const struct tetra_rxcore *c, uint32_t idx)
{
    return c->ring[idx & (TETRA_RING_BITS - 1)];
}

/** Copy n soft bits starting at absolute index idx. */
void rxcore_copy(const struct tetra_rxcore *c, uint32_t idx, int8_t *out,
                 unsigned n);

/** Number of bit errors of the last 38 received bits against y. */
unsigned rxcore_sync_errors(const struct tetra_rxcore *c);

/**
 * Soft correlation of a reference sequence at absolute index idx,
 * normalised to the range -1000..1000.
 */
int rxcore_corr(const struct tetra_rxcore *c, uint32_t idx, const uint8_t *seq,
                unsigned n);

/** Convert hard dibits / bits to soft values and push them. */
void rxcore_push_dibit(struct tetra_rxcore *c, uint8_t d);

#endif /* TETRA_L1_H */
