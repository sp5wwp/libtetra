/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * Block codes, interleaving and scrambling.
 * EN 300 392-2 clauses 8.2.3 - 8.2.5, EN 300 396-2 clauses 8.2.3 - 8.2.5.
 */

#include <string.h>
#include <tetra/tetra_coding.h>

/* G(X) = X^16 + X^12 + X^5 + 1, CCITT polynomial. */
#define CRC16_POLY 0x1021

uint16_t tetra_crc16(const uint8_t *bits, unsigned k1)
{
    /*
     * F(X) = [X^16 M(X) + X^K1 * sum(X^i, i = 0..15)] mod G(X) + sum(X^i):
     * a register preset to all ones, then the remainder complemented.
     */
    uint16_t crc = 0xffff;

    for (unsigned i = 0; i < k1; i++) {
        unsigned fb = ((crc >> 15) ^ bits[i]) & 1;

        crc <<= 1;
        if (fb)
            crc ^= CRC16_POLY;
    }
    return (uint16_t)~crc;
}

void tetra_crc16_append(uint8_t *bits, unsigned k1)
{
    uint16_t crc = tetra_crc16(bits, k1);

    for (unsigned i = 0; i < 16; i++)
        bits[k1 + i] = (crc >> (15 - i)) & 1;
}

bool tetra_crc16_check(const uint8_t *bits, unsigned k1)
{
    uint16_t crc = tetra_crc16(bits, k1);

    for (unsigned i = 0; i < 16; i++) {
        if (bits[k1 + i] != ((crc >> (15 - i)) & 1))
            return false;
    }
    return true;
}

/*
 * FCS: same generator as the scrambler, first 32 bits complemented,
 * remainder complemented. This is the non-reflected CRC-32.
 */
#define FCS32_POLY 0x04c11db7u

uint32_t tetra_fcs32(const uint8_t *bits, unsigned n)
{
    uint32_t crc = 0xffffffffu;

    for (unsigned i = 0; i < n; i++) {
        unsigned fb = ((crc >> 31) ^ bits[i]) & 1;

        crc <<= 1;
        if (fb)
            crc ^= FCS32_POLY;
    }
    return ~crc;
}

uint32_t tetra_fcs32_packed(const uint8_t *buf, unsigned start, unsigned n)
{
    uint32_t crc = 0xffffffffu;

    for (unsigned i = start; i < start + n; i++) {
        unsigned bit = (buf[i >> 3] >> (7 - (i & 7))) & 1;
        unsigned fb = ((crc >> 31) ^ bit) & 1;

        crc <<= 1;
        if (fb)
            crc ^= FCS32_POLY;
    }
    return ~crc;
}

void tetra_interleave(const uint8_t *in, uint8_t *out, unsigned k, unsigned a)
{
    unsigned idx = 0;

    /* b4(1 + (a * i) mod K) = b3(i), i = 1..K */
    for (unsigned i = 0; i < k; i++) {
        idx += a;
        while (idx >= k)
            idx -= k;
        out[idx] = in[i];
    }
}

void tetra_deinterleave_soft(const int8_t *in, int8_t *out, unsigned k,
                             unsigned a)
{
    unsigned idx = 0;

    for (unsigned i = 0; i < k; i++) {
        idx += a;
        while (idx >= k)
            idx -= k;
        out[i] = in[idx];
    }
}

/*
 * Scrambler, EN 300 392-2 clause 8.2.5.2.
 * State bit (j - 1) holds p(k - j). Initially p(0) = e(1) ... p(-29) = e(30)
 * and p(-30) = p(-31) = 1.
 */
#define SCRAMB_TAPS                                                          \
    ((1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 6) | (1u << 7) | \
     (1u << 9) | (1u << 10) | (1u << 11) | (1u << 15) | (1u << 21) |         \
     (1u << 22) | (1u << 25) | (1u << 31))

uint32_t tetra_scramb_init(uint32_t code)
{
    uint32_t s = 3u << 30;

    for (unsigned i = 0; i < 30; i++) {
        /* e(i + 1) is bit (29 - i) of code, goes to state bit i */
        if (code & (1u << (29 - i)))
            s |= 1u << i;
    }
    return s;
}

static inline unsigned parity32(uint32_t v)
{
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1;
}

static inline unsigned scramb_next(uint32_t *s)
{
    unsigned p = parity32(*s & SCRAMB_TAPS);

    *s = (*s << 1) | p;
    return p;
}

void tetra_scramble(uint8_t *bits, unsigned n, uint32_t init)
{
    uint32_t s = init;

    for (unsigned i = 0; i < n; i++)
        bits[i] ^= (uint8_t)scramb_next(&s);
}

void tetra_descramble_soft(int8_t *soft, unsigned n, uint32_t init)
{
    uint32_t s = init;

    for (unsigned i = 0; i < n; i++) {
        if (scramb_next(&s))
            soft[i] = (int8_t)(soft[i] == -128 ? 127 : -soft[i]);
    }
}

uint32_t tetra_tmo_colour_code(uint16_t mcc, uint16_t mnc, uint8_t cc)
{
    return ((uint32_t)(mcc & 0x3ff) << 20) | ((uint32_t)(mnc & 0x3fff) << 6) |
           (cc & 0x3f);
}

uint32_t tetra_dmo_colour_code(uint32_t mni, uint32_t src)
{
    return ((mni & 0x3f) << 24) | (src & 0xffffff);
}

/*
 * Shortened (30,14) Reed-Muller code, EN 300 392-2 clause 8.2.3.2.
 * G = [I14 | P]; each entry is one row of P (16 bits, first column MSB).
 */
static const uint16_t rm_p[14] = {
    0x9b60, 0x2de0, 0xfc20, 0xe03c, 0x983a, 0x5436, 0x2c2e,
    0xffdf, 0x8339, 0x42b5, 0x21ad, 0x1273, 0x096b, 0x04e7,
};

static uint16_t rm_parity(uint16_t data)
{
    uint16_t par = 0;

    for (unsigned i = 0; i < 14; i++) {
        if (data & (1u << (13 - i)))
            par ^= rm_p[i];
    }
    return par;
}

uint32_t tetra_rm3014_encode(uint16_t data)
{
    data &= 0x3fff;
    return ((uint32_t)data << 16) | rm_parity(data);
}

/* Syndrome contribution of a single error at codeword bit position i. */
static uint16_t rm_err_syndrome(unsigned i)
{
    /* i = 0 is b2(1), the MSB of the data part */
    if (i < 14)
        return rm_p[i];
    return (uint16_t)(1u << (29 - i));
}

int tetra_rm3014_decode(uint32_t cw, uint16_t *data)
{
    uint16_t d = (cw >> 16) & 0x3fff;
    uint16_t syn = rm_parity(d) ^ (uint16_t)(cw & 0xffff);

    if (!syn) {
        *data = d;
        return 0;
    }
    for (unsigned i = 0; i < 30; i++) {
        if (rm_err_syndrome(i) == syn) {
            if (i < 14)
                d ^= (uint16_t)(1u << (13 - i));
            *data = d;
            return 1;
        }
    }
    for (unsigned i = 0; i < 30; i++) {
        uint16_t si = rm_err_syndrome(i);

        for (unsigned j = i + 1; j < 30; j++) {
            if ((si ^ rm_err_syndrome(j)) != syn)
                continue;
            if (i < 14)
                d ^= (uint16_t)(1u << (13 - i));
            if (j < 14)
                d ^= (uint16_t)(1u << (13 - j));
            *data = d;
            return 2;
        }
    }
    return -1;
}

void tetra_soft_to_bits(const int8_t *soft, uint8_t *bits, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        bits[i] = soft[i] > 0;
}

void tetra_pack_bits(const uint8_t *bits, unsigned n, uint8_t *out)
{
    memset(out, 0, (n + 7) / 8);
    for (unsigned i = 0; i < n; i++) {
        if (bits[i])
            out[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    }
}
