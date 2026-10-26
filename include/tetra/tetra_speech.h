/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_SPEECH_H
#define TETRA_SPEECH_H

/*
 * Channel decoding for speech (TCH/S), EN 300 395-2 clauses 5 and 6.
 *
 * Turns a traffic slot (TETRA_EV_TRAFFIC) into two 137-bit speech frames
 * with a bad frame indicator each, ready for an ACELP speech decoder such
 * as OpenACELP's oa_dec_frame(). Applies to both TMO and DMO.
 */

#include <stdbool.h>
#include <stdint.h>
#include <tetra/tetra_common.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bits per speech frame (30 ms). */
#define TETRA_SPEECH_BITS 137

/** Workspace for the decoder. Allocate statically; contents are private. */
struct tetra_speech_ws {
    int8_t t3[432];
    int8_t mother[3 * 184];
    uint8_t t2[286];
    uint16_t dec[184];
};

/** The two speech frames of a traffic slot. */
struct tetra_speech {
    /** Frame unusable (stolen or CRC failure): run error concealment. */
    bool bfi[2];
    /** Bits B1..B137 of each frame, one bit per byte, in codec order. */
    uint8_t bits[2][TETRA_SPEECH_BITS];
};

/**
 * \brief Decode the speech frames of a traffic slot.
 *
 * Handles normal slots (two frames coded together) and slots whose first
 * half was stolen (one half-slot frame). Frames that are not available are
 * marked with bfi and their bits cleared.
 *
 * @param ws: workspace.
 * @param t: traffic event.
 * @param out: decoded frames; out->bits[0] is the earlier frame.
 * @return number of frames that passed the CRC (0..2).
 */
int tetra_speech_decode(struct tetra_speech_ws *ws,
                        const struct tetra_traffic *t,
                        struct tetra_speech *out);

/**
 * \brief Decode a full speech slot (EN 300 395-2 clause 5.5).
 *
 * @param ws: workspace.
 * @param type4: 432 descrambled soft type-4 bits.
 * @param a: first speech frame, 137 bits.
 * @param b: second speech frame, 137 bits.
 * @return true if the CRC and parity bits match.
 */
bool tetra_speech_decode_slot(struct tetra_speech_ws *ws, const int8_t *type4,
                              uint8_t *a, uint8_t *b);

/**
 * \brief Decode a half-slot speech frame (EN 300 395-2 clause 5.6).
 *
 * @param ws: workspace.
 * @param type4: 216 descrambled soft type-4 bits of block 2.
 * @param a: speech frame, 137 bits.
 * @return true if the CRC matches.
 */
bool tetra_speech_decode_half(struct tetra_speech_ws *ws, const int8_t *type4,
                              uint8_t *a);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_SPEECH_H */
