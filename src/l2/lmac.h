/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_LMAC_H
#define TETRA_LMAC_H

/*
 * Lower MAC: error control schemes of the signalling channels.
 * EN 300 392-2 clause 8.3.1, EN 300 396-2 clause 8.3.1.
 */

#include <stdbool.h>
#include <stdint.h>
#include <tetra/tetra_common.h>

/* Signalling channel classes sharing the same error control scheme. */
enum lmac_chan {
    LMAC_SCH_S = 0, /* SCH/S, BSCH: 60 -> 120 bits, (120,11) */
    LMAC_SCH_H,     /* SCH/H, SCH/HD, BNCH, STCH: 124 -> 216, (216,101) */
    LMAC_SCH_F,     /* SCH/F: 268 -> 432, (432,103) */
};

struct lmac_chan_def {
    uint16_t k1; /* type-1 bits */
    uint16_t k5; /* type-5 bits */
    uint16_t a;  /* interleaver parameter */
};

extern const struct lmac_chan_def lmac_chan_defs[3];

/**
 * Decode one signalling block.
 *
 * @param ws: workspace.
 * @param ch: channel class.
 * @param type5: received soft type-5 bits.
 * @param scramb: scrambler state from tetra_scramb_init().
 * @param out: packed type-1 bits, (k1 + 7) / 8 bytes.
 * @return true if the CRC matched.
 */
bool lmac_decode(struct tetra_lmac_ws *ws, enum lmac_chan ch,
                 const int8_t *type5, uint32_t scramb, uint8_t *out);

/**
 * Encode one signalling block (for tests and transmitters).
 *
 * @param ch: channel class.
 * @param type1: k1 unpacked type-1 bits.
 * @param scramb: scrambler state.
 * @param type5: k5 output bits.
 */
void lmac_encode(enum lmac_chan ch, const uint8_t *type1, uint32_t scramb,
                 uint8_t *type5);

/**
 * Viterbi decoder of the rate 1/4 RCPC mother code (4 soft values per
 * step), ending in state 0.
 */
void viterbi_rcpc(const int8_t *mother, unsigned k2, uint8_t *out,
                  uint16_t *dec);

/**
 * Viterbi decoder of the rate 1/3 speech code (3 soft values per step),
 * ending in state 0.
 */
void viterbi_speech(const int8_t *mother, unsigned k2, uint8_t *out,
                    uint16_t *dec);

/** Speech code output (G1 in bit 2 .. G3 in bit 0) for register r. */
uint8_t conv_out3(unsigned r);

/** Mother code output (V1 in bit 3 .. V4 in bit 0) for register r. */
uint8_t conv_out_nibble(unsigned r);

/**
 * Descramble soft bits in place, using the scrambling sequence cache of the
 * workspace.
 */
void lmac_descramble(struct tetra_lmac_ws *ws, int8_t *soft, unsigned n,
                     uint32_t scramb);

#endif /* TETRA_LMAC_H */
