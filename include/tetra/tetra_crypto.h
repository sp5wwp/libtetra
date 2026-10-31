/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_CRYPTO_H
#define TETRA_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tetra/tetra_common.h>

#ifdef __cplusplus
extern "C" {
#endif

/** TEA key size, in bytes (80 bits). */
#define TETRA_TEA_KEY_BYTES 10

/**
 * Derive the 80-bit encryption key (ECK) from a 80-bit base key (CK)
 * using TB5 and the serving cell network parameters.
 *
 * @param cn 12-bit main carrier number.
 * @param la 14-bit location area.
 * @param cc 6-bit colour code.
 * @param ck 80-bit provisioned base key, in network byte order.
 * @param eck receives the derived 80-bit key.
 * @return true on success; false for invalid fields or pointers.
 */
bool tetra_tb5(uint16_t cn, uint16_t la, uint8_t cc,
               const uint8_t ck[TETRA_TEA_KEY_BYTES],
               uint8_t eck[TETRA_TEA_KEY_BYTES]);

/** TETRA air-interface keystream generators. */
enum tetra_tea_algorithm {
    TETRA_TEA1 = 1,
    TETRA_TEA2 = 2,
    TETRA_TEA3 = 3,
};

/** Direction bit used when constructing a TMO cipher IV. */
enum tetra_tea_direction {
    TETRA_TEA_DOWNLINK = 0,
    TETRA_TEA_UPLINK = 1,
};

/**
 * Build the 29-bit TMO cipher IV (EN 300 392-7, clause 6.3).
 *
 * The time fields use the values carried by TETRA: timeslot 1..4, frame
 * 1..18, and multiframe 1..60. Only the low 15 bits of hyperframe are placed
 * in the IV. The resulting bit layout, from least to most significant, is
 * TN-1 (2), FN (5), MN (6), hyperframe extension (15), direction (1).
 *
 * @param time TDMA position; must contain valid TMO time fields.
 * @param hyperframe hyperframe number; only bits 0..14 are used.
 * @param direction TETRA_TEA_DOWNLINK or TETRA_TEA_UPLINK.
 * @param iv receives the 29-bit IV.
 * @return true on success, false for invalid arguments or time fields.
 */
bool tetra_tea_build_iv(const struct tetra_time *time, uint16_t hyperframe,
                        enum tetra_tea_direction direction, uint32_t *iv);

/**
 * Generate TEA1, TEA2, or TEA3 keystream bytes.
 *
 * @param algorithm generator to use.
 * @param iv 29-bit TETRA cipher IV, as produced by tetra_tea_build_iv().
 * @param key 80-bit encryption key, in network byte order.
 * @param output_len number of keystream bytes to generate.
 * @param output receives output_len bytes; may be NULL only when output_len
 *               is zero.
 * @return true on success; false for an unknown algorithm, invalid IV, or
 *         invalid pointers.
 *
 * The returned keystream is byte-oriented. For bit-oriented air-interface
 * fields, consume the most-significant bit of each byte first.
 */
bool tetra_tea_generate(enum tetra_tea_algorithm algorithm, uint32_t iv,
                        const uint8_t key[TETRA_TEA_KEY_BYTES],
                        size_t output_len, uint8_t *output);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_CRYPTO_H */
