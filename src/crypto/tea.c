/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <tetra/tetra_crypto.h>

#include "tea_internal.h"

bool tetra_tea_build_iv(const struct tetra_time *time, uint16_t hyperframe,
                        enum tetra_tea_direction direction, uint32_t *iv)
{
    if (!time || !iv || time->tn < 1 || time->tn > 4 || time->fn < 1 ||
        time->fn > 18 || time->mn < 1 || time->mn > 60 ||
        (direction != TETRA_TEA_DOWNLINK && direction != TETRA_TEA_UPLINK))
        return false;

    *iv = (uint32_t)(time->tn - 1) | ((uint32_t)time->fn << 2) |
          ((uint32_t)time->mn << 7) |
          ((uint32_t)(hyperframe & 0x7fff) << 13) |
          ((uint32_t)direction << 28);
    return true;
}

bool tetra_tea_generate(enum tetra_tea_algorithm algorithm, uint32_t iv,
                        const uint8_t key[TETRA_TEA_KEY_BYTES],
                        size_t output_len, uint8_t *output)
{
    if (!key || (output_len && !output) || (iv & ~0x1fffffffu))
        return false;

    switch (algorithm) {
    case TETRA_TEA1:
        tetra_tea1_raw(iv, key, output_len, output);
        return true;
    case TETRA_TEA2:
        tetra_tea2_raw(iv, key, output_len, output);
        return true;
    case TETRA_TEA3:
        tetra_tea3_raw(iv, key, output_len, output);
        return true;
    default:
        return false;
    }
}
