/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * TA61 identity decryption glue.
 *
 * Packs the 24-bit short identity into three bytes, applies the vendored
 * osmo-tetra TA61 inverse (src/crypto/taa1.c, AGPL-3.0-or-later) with the
 * cell CCK, and unpacks the cleartext SSI. Mirrors osmo-tetra's
 * decrypt_identity().
 */

#include <tetra/tetra_crypto.h>

#if TETRA_ENABLE_CRYPTO

#include "taa1.h"

bool tetra_ta61_decrypt_ssi(const uint8_t cck[TETRA_TEA_KEY_BYTES],
                            uint32_t esi, uint32_t *ssi)
{
    uint8_t key[TETRA_TEA_KEY_BYTES];
    uint8_t id[3];

    if (!cck || !ssi)
        return false;

    id[0] = (uint8_t)(esi >> 16);
    id[1] = (uint8_t)(esi >> 8);
    id[2] = (uint8_t)esi;

    /* ta61_inv() takes a non-const key pointer but does not modify it; use
     * a local copy so the caller's key can be const. */
    for (unsigned i = 0; i < TETRA_TEA_KEY_BYTES; i++)
        key[i] = cck[i];

    ta61_inv(key, id, id);

    *ssi = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
    return true;
}

#endif /* TETRA_ENABLE_CRYPTO */
