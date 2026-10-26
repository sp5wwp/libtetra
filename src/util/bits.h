/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_BITS_H
#define TETRA_BITS_H

/* Packed bit buffers, MSB first, as used for all PDU parsing. */

#include <stdbool.h>
#include <stdint.h>

struct bitrd {
    const uint8_t *buf;
    uint16_t pos;
    uint16_t end;
    bool err; /* set when reading past the end */
};

static inline void br_init(struct bitrd *b, const uint8_t *buf, unsigned start,
                           unsigned end)
{
    b->buf = buf;
    b->pos = (uint16_t)start;
    b->end = (uint16_t)end;
    b->err = false;
}

static inline unsigned br_left(const struct bitrd *b)
{
    return b->pos < b->end ? (unsigned)(b->end - b->pos) : 0;
}

static inline unsigned bit_at(const uint8_t *buf, unsigned i)
{
    return (buf[i >> 3] >> (7 - (i & 7))) & 1;
}

/** Read n bits (n <= 32), MSB first. Returns 0 and sets err on overrun. */
uint32_t br_get(struct bitrd *b, unsigned n);

/** Skip n bits. */
void br_skip(struct bitrd *b, unsigned n);

/** Write n bits (n <= 32) of v at bit position *pos. */
void bw_put(uint8_t *buf, unsigned *pos, uint32_t v, unsigned n);

/** Copy n bits between packed buffers. */
void bits_copy(uint8_t *dst, unsigned dpos, const uint8_t *src, unsigned spos,
               unsigned n);

/**
 * Remove fill bits from the end of [start, end): the last '1' and any
 * following zeros. Returns the new end, or start if no '1' is found.
 */
unsigned bits_strip_fill(const uint8_t *buf, unsigned start, unsigned end);

#endif /* TETRA_BITS_H */
