/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "bits.h"

uint32_t br_get(struct bitrd *b, unsigned n)
{
    uint32_t v = 0;

    if (b->pos + n > b->end) {
        b->err = true;
        b->pos = b->end;
        return 0;
    }
    for (unsigned i = 0; i < n; i++)
        v = (v << 1) | bit_at(b->buf, b->pos + i);
    b->pos = (uint16_t)(b->pos + n);
    return v;
}

void br_skip(struct bitrd *b, unsigned n)
{
    if (b->pos + n > b->end) {
        b->err = true;
        b->pos = b->end;
        return;
    }
    b->pos = (uint16_t)(b->pos + n);
}

void bw_put(uint8_t *buf, unsigned *pos, uint32_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        unsigned p = *pos + i;
        uint8_t m = (uint8_t)(0x80 >> (p & 7));

        if ((v >> (n - 1 - i)) & 1)
            buf[p >> 3] |= m;
        else
            buf[p >> 3] &= (uint8_t)~m;
    }
    *pos += n;
}

void bits_copy(uint8_t *dst, unsigned dpos, const uint8_t *src, unsigned spos,
               unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        unsigned p = dpos + i;
        uint8_t m = (uint8_t)(0x80 >> (p & 7));

        if (bit_at(src, spos + i))
            dst[p >> 3] |= m;
        else
            dst[p >> 3] &= (uint8_t)~m;
    }
}

unsigned bits_strip_fill(const uint8_t *buf, unsigned start, unsigned end)
{
    while (end > start) {
        end--;
        if (bit_at(buf, end))
            return end;
    }
    return start;
}
