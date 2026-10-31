/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <string.h>
#include <tetra/tetra_crypto.h>
#include "tmo_int.h"

#define TMO_CRYPTO_MAX_BITS (TETRA_TMO_MAX_SDU_BITS + 216u)

bool tmo_crypto_get_eck(struct tetra_tmo_rx *rx,
                        const struct tetra_tmo_key_request *request,
                        enum tetra_tea_algorithm *algorithm,
                        uint8_t eck[TETRA_TEA_KEY_BYTES])
{
    uint8_t ck[TETRA_TEA_KEY_BYTES] = { 0 };
    bool ok;

    if (!rx || !request || !algorithm || !eck || !rx->key_provider ||
        !rx->cell.have_sysinfo || !rx->cell.have_hyperframe)
        return false;

    ok = rx->key_provider(rx->key_provider_user, request, algorithm, ck);
    if (ok)
        ok = tetra_tb5(rx->cell.main_carrier, rx->cell.location_area,
                       rx->cell.colour_code, ck, eck);
    memset(ck, 0, sizeof(ck));
    return ok;
}

bool tmo_crypto_keystream_bits(struct tetra_tmo_rx *rx,
                               enum tetra_tea_algorithm algorithm,
                               const uint8_t eck[TETRA_TEA_KEY_BYTES],
                               unsigned bit_offset, unsigned nbits,
                               uint8_t *output)
{
    uint32_t iv;
    unsigned total;
    unsigned nbytes;
    uint8_t stream[(TMO_CRYPTO_MAX_BITS + 7u) / 8u];

    if (!rx || !eck || (!output && nbits) ||
        bit_offset > TMO_CRYPTO_MAX_BITS ||
        nbits > TMO_CRYPTO_MAX_BITS - bit_offset)
        return false;
    total = bit_offset + nbits;
    if (!tetra_tea_build_iv(&rx->t, rx->cell.hyperframe,
                            TETRA_TEA_DOWNLINK, &iv))
        return false;
    nbytes = (total + 7u) / 8u;
    if (!tetra_tea_generate(algorithm, iv, eck, nbytes, stream))
        return false;
    for (unsigned i = 0; i < nbits; i++)
        output[i] = (uint8_t)bit_at(stream, bit_offset + i);
    return true;
}

bool tmo_crypto_xor_bits(struct tetra_tmo_rx *rx,
                         enum tetra_tea_algorithm algorithm,
                         const uint8_t eck[TETRA_TEA_KEY_BYTES],
                         uint8_t *buf, unsigned start, unsigned nbits,
                         unsigned bit_offset)
{
    uint32_t iv;
    unsigned total;
    unsigned nbytes;
    /* Keystream is already packed: one bit per bit, not one byte per bit.
     * Max stack use is therefore 356 bytes for the largest configured SDU. */
    uint8_t stream[(TMO_CRYPTO_MAX_BITS + 7u) / 8u];

    if (!rx || !eck || !buf || start > TETRA_TMO_MAX_SDU_BITS ||
        nbits > TETRA_TMO_MAX_SDU_BITS - start ||
        bit_offset > TMO_CRYPTO_MAX_BITS ||
        nbits > TMO_CRYPTO_MAX_BITS - bit_offset)
        return false;

    total = bit_offset + nbits;
    nbytes = (total + 7u) / 8u;
    if (!tetra_tea_build_iv(&rx->t, rx->cell.hyperframe,
                            TETRA_TEA_DOWNLINK, &iv) ||
        !tetra_tea_generate(algorithm, iv, eck, nbytes, stream))
        return false;

    for (unsigned i = 0; i < nbits; i++) {
        unsigned p = start + i;
        if (bit_at(stream, bit_offset + i)) {
            uint8_t mask = (uint8_t)(0x80u >> (p & 7));
            buf[p >> 3] ^= mask;
        }
    }
    return true;
}
