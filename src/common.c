/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <string.h>
#include <tetra/tetra_common.h>

uint32_t tetra_carrier_hz(uint8_t band, uint16_t carrier, uint8_t offset)
{
    static const int32_t off_hz[4] = { 0, 6250, -6250, 12500 };

    return (uint32_t)((int32_t)band * 100000000 + (int32_t)carrier * 25000 +
                      off_hz[offset & 3]);
}

uint32_t tetra_uplink_hz(uint32_t dl_hz, uint8_t band, uint8_t duplex_spacing,
                         bool reverse)
{
    /* Duplex spacing in kHz, TS 100 392-15 table 2; R is reserved. */
#define R 0xffffu
    static const uint16_t spacing_khz[8][16] = {
        { R, 1600, 10000, 10000, 10000, 10000, 10000, R, R, R, R, R, R, R, R,
          R },
        { R, 4500, R, 36000, 7000, R, R, R, 45000, 45000, R, R, R, R, R, R },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { R, R, R, 8000, 8000, R, R, R, 18000, 18000, R, R, R, R, R, R },
        { R, R, R, 18000, 5000, R, 30000, 30000, R, 39000, R, R, R, R, R, R },
        { R, R, R, R, 9500, R, R, R, R, R, R, R, R, R, R, R },
        { R, R, R, R, R, R, R, R, R, R, R, R, R, R, R, R },
        { R, R, R, R, R, R, R, R, R, R, R, R, R, R, R, R },
    };
    unsigned khz = spacing_khz[duplex_spacing & 7][band & 15];
#undef R
    uint32_t d;

    if (khz == 0xffffu || !dl_hz)
        return 0;
    d = (uint32_t)khz * 1000u;
    if (reverse)
        return dl_hz + d;
    return d < dl_hz ? dl_hz - d : 0;
}

static int16_t etsi_soft(int8_t s)
{
    /* '1' maps to negative values; scale the +-64 range to +-127 */
    int v = -2 * (int)s;

    if (v > 127)
        v = 127;
    if (v < -127)
        v = -127;
    return (int16_t)v;
}

void tetra_traffic_to_etsi(const struct tetra_traffic *t, int16_t out[690])
{
    static const uint16_t seg_len[4] = { 114, 114, 114, 90 };
    const int8_t *src = t->bits;
    unsigned n = TETRA_TCH_BITS, k = 0;

    memset(out, 0, 690 * sizeof(out[0]));
    for (unsigned i = 0; i < 6; i++)
        out[115 * i] = (int16_t)(0x6b21 + i);

    if (t->half[0] != TETRA_HALF_TCH) {
        src = t->bits + 216;
        n = 216;
    }
    for (unsigned seg = 0; seg < 4 && k < n; seg++) {
        for (unsigned i = 0; i < seg_len[seg] && k < n; i++)
            out[115 * seg + 1 + i] = etsi_soft(src[k++]);
    }
}
