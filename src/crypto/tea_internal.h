/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_TEA_INTERNAL_H
#define TETRA_TEA_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

void tetra_tea1_raw(uint32_t iv, const uint8_t *key, size_t output_len,
                    uint8_t *output);
void tetra_tea2_raw(uint32_t iv, const uint8_t *key, size_t output_len,
                    uint8_t *output);
void tetra_tea3_raw(uint32_t iv, const uint8_t *key, size_t output_len,
                    uint8_t *output);

#endif /* TETRA_TEA_INTERNAL_H */
