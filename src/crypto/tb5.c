/*
 * TB5 key derivation, based on the bit-mask definition implemented by
 * Osmocom osmo-tetra (src/crypto/taa1.c).
 * Copyright (C) 2023 Midnight Blue B.V.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <tetra/tetra_crypto.h>

bool tetra_tb5(uint16_t cn, uint16_t la, uint8_t cc,
               const uint8_t ck[TETRA_TEA_KEY_BYTES],
               uint8_t eck[TETRA_TEA_KEY_BYTES])
{
    uint8_t mask[TETRA_TEA_KEY_BYTES] = { 0 };
    unsigned pos = 0;
    const uint16_t fields[] = { la, cn, cc, cn, cc, cn, cc, cn };
    const uint8_t widths[] = { 14, 12, 6, 12, 6, 12, 6, 12 };

    if (!ck || !eck || cn > 0x0fff || la > 0x3fff || cc > 0x3f)
        return false;

    /* TB5 mask: LA:14 | CN:12 | CC:6 | CN:12 | CC:6 | CN:12 | CC:6 | CN:12. */
    for (unsigned f = 0; f < sizeof(widths) / sizeof(widths[0]); f++) {
        for (unsigned i = 0; i < widths[f]; i++, pos++) {
            unsigned bit = (fields[f] >> (widths[f] - 1 - i)) & 1u;
            if (bit)
                mask[pos >> 3] |= (uint8_t)(0x80u >> (pos & 7));
        }
    }

    for (unsigned i = 0; i < TETRA_TEA_KEY_BYTES; i++)
        eck[i] = ck[i] ^ mask[i];
    return true;
}
