/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_TX_H
#define TETRA_TX_H

/*
 * Transmit-side building blocks: channel encoding and burst assembly.
 *
 * No transmitter state machine is provided yet; these functions produce
 * the exact bit sequences of the bursts and are used by the tests and the
 * signal generator. All arrays hold one bit per byte.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Signalling channel classes. */
enum tetra_sch {
    TETRA_SCH_S = 0, /**< SCH/S (DMO), BSCH (TMO): 60 type-1 bits. */
    TETRA_SCH_H,     /**< SCH/H, SCH/HD, BNCH, STCH: 124 type-1 bits. */
    TETRA_SCH_F,     /**< SCH/F: 268 type-1 bits. */
};

/** DMO burst length (bits) excluding guard and ramping periods. */
#define TETRA_DMO_BURST_BITS 470
/** TMO downlink slot length in bits. */
#define TETRA_SLOT_BITS 510

/**
 * \brief Encode a signalling block (CRC, RCPC 2/3, interleaving,
 * scrambling).
 *
 * @param ch: channel class.
 * @param type1: type-1 bits (60, 124 or 268).
 * @param scramb: scrambler state from tetra_scramb_init().
 * @param type5: output type-5 bits (120, 216 or 432).
 */
void tetra_encode_sch(enum tetra_sch ch, const uint8_t *type1, uint32_t scramb,
                      uint8_t *type5);

/**
 * \brief Encode a TMO AACH (ACCESS-ASSIGN) block.
 *
 * @param data: 14-bit ACCESS-ASSIGN PDU, first bit in bit 13.
 * @param scramb: scrambler state.
 * @param bb: 30 output bits.
 */
void tetra_encode_aach(uint16_t data, uint32_t scramb, uint8_t bb[30]);

/**
 * \brief Build a DM synchronisation burst (EN 300 396-2 table 16).
 *
 * @param burst: 470 output bits.
 * @param sb: 120 bits of SCH/S.
 * @param bkn2: 216 bits of SCH/H.
 */
void tetra_dmo_build_dsb(uint8_t burst[470], const uint8_t sb[120],
                         const uint8_t bkn2[216]);

/**
 * \brief Build a DM normal burst (EN 300 396-2 table 15).
 *
 * @param burst: 470 output bits.
 * @param bkn1: 216 bits of block 1.
 * @param bkn2: 216 bits of block 2.
 * @param stch: true to use preamble P2 / training sequence 2 (STCH).
 */
void tetra_dmo_build_dnb(uint8_t burst[470], const uint8_t bkn1[216],
                         const uint8_t bkn2[216], bool stch);

/**
 * \brief Build a TMO normal continuous downlink burst (table 9.7).
 *
 * @param slot: 510 output bits.
 * @param bkn1: 216 bits of block 1.
 * @param bb: 30 bits of the broadcast block (AACH).
 * @param bkn2: 216 bits of block 2.
 * @param two_blocks: training sequence 2 (two half-slot channels).
 */
void tetra_tmo_build_ndb(uint8_t slot[510], const uint8_t bkn1[216],
                         const uint8_t bb[30], const uint8_t bkn2[216],
                         bool two_blocks);

/**
 * \brief Build a TMO synchronisation continuous downlink burst (table 9.9).
 *
 * @param slot: 510 output bits.
 * @param sb: 120 bits of BSCH.
 * @param bb: 30 bits of the broadcast block (AACH).
 * @param bkn2: 216 bits of block 2.
 */
void tetra_tmo_build_sb(uint8_t slot[510], const uint8_t sb[120],
                        const uint8_t bb[30], const uint8_t bkn2[216]);

/**
 * \brief Channel encode two speech frames into a traffic slot
 * (EN 300 395-2 clause 5.5).
 *
 * @param a: first speech frame, 137 bits B1..B137.
 * @param b: second speech frame, 137 bits.
 * @param type4: 432 output type-4 bits (to be scrambled).
 */
void tetra_speech_encode_slot(const uint8_t *a, const uint8_t *b,
                              uint8_t *type4);

/**
 * \brief Channel encode one speech frame for block 2 of a slot whose first
 * half is stolen (EN 300 395-2 clause 5.6).
 *
 * @param a: speech frame, 137 bits B1..B137.
 * @param type4: 216 output type-4 bits (to be scrambled).
 */
void tetra_speech_encode_half(const uint8_t *a, uint8_t *type4);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_TX_H */
