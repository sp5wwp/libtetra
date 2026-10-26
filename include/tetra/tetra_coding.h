/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_CODING_H
#define TETRA_CODING_H

/*
 * Channel coding primitives shared by TMO and DMO
 * (EN 300 392-2 clause 8, EN 300 396-2 clause 8).
 *
 * Conventions:
 *  - "bits" arrays hold one bit per byte (0 or 1), index 0 is the first
 *    transmitted bit (the spec's bit number 1).
 *  - "soft" arrays hold one int8_t per bit; a value > 0 means '1', < 0 means
 *    '0' and the magnitude is the confidence. 0 is an erasure.
 *  - "packed" arrays hold 8 bits per byte, MSB first.
 *
 * These are public so they can be unit tested and reused by a future
 * transmitter. Receivers normally use the higher level APIs instead.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Soft bit value used for a hard '1'. */
#define TETRA_SOFT_ONE 64
/** Soft bit value used for a hard '0'. */
#define TETRA_SOFT_ZERO (-64)

/** Puncturing schemes of the 16-state RCPC code. */
enum tetra_punct {
    TETRA_PUNCT_2_3 = 0, /**< Rate 2/3: SCH/S, SCH/H, SCH/F, BSCH, STCH. */
    TETRA_PUNCT_292_432, /**< Rate 292/432: TCH/4,8. */
    TETRA_PUNCT_148_432, /**< Rate 148/432: TCH/2,4. */
};

/**
 * \brief Compute the (K1+16, K1) block code check bits.
 *
 * @param bits: K1 type-1 bits.
 * @param k1: number of type-1 bits.
 * @return the 16 check bits, b2(K1+1) in bit 15 down to b2(K1+16) in bit 0.
 */
uint16_t tetra_crc16(const uint8_t *bits, unsigned k1);

/**
 * \brief Append the 16 check bits after K1 type-1 bits.
 *
 * @param bits: buffer of at least K1 + 16 bits.
 * @param k1: number of type-1 bits.
 */
void tetra_crc16_append(uint8_t *bits, unsigned k1);

/**
 * \brief Verify the 16 check bits following K1 type-1 bits.
 *
 * @param bits: K1 + 16 type-2 bits.
 * @param k1: number of type-1 bits.
 * @return true if the check bits match.
 */
bool tetra_crc16_check(const uint8_t *bits, unsigned k1);

/**
 * \brief Compute the 32-bit FCS (EN 300 392-2 annex C, EN 300 396-3 annex B).
 *
 * @param bits: input bits.
 * @param n: number of input bits.
 * @return the FCS, coefficient of x^31 in bit 31.
 */
uint32_t tetra_fcs32(const uint8_t *bits, unsigned n);

/**
 * \brief Compute the 32-bit FCS over a packed bit buffer.
 *
 * @param buf: packed input, MSB first.
 * @param start: index of the first bit.
 * @param n: number of bits.
 * @return the FCS.
 */
uint32_t tetra_fcs32_packed(const uint8_t *buf, unsigned start, unsigned n);

/**
 * \brief Encode with the 16-state rate 1/4 mother code.
 *
 * @param in: K2 type-2 bits (including tail bits).
 * @param k2: number of type-2 bits.
 * @param mother: output, 4 * K2 bits.
 */
void tetra_conv_encode(const uint8_t *in, unsigned k2, uint8_t *mother);

/**
 * \brief Select K3 type-3 bits from the mother code output.
 *
 * @param p: puncturing scheme.
 * @param mother: 4 * K2 mother code bits.
 * @param out: K3 output bits.
 * @param k3: number of type-3 bits to produce.
 */
void tetra_puncture(enum tetra_punct p, const uint8_t *mother, uint8_t *out,
                    unsigned k3);

/**
 * \brief Map K3 soft type-3 bits back onto the mother code positions.
 *
 * Positions removed by puncturing are set to 0 (erasure).
 *
 * @param p: puncturing scheme.
 * @param in: K3 soft bits.
 * @param k3: number of type-3 bits.
 * @param mother: output, 4 * K2 soft bits (cleared by this function).
 * @param k2: number of type-2 bits.
 */
void tetra_depuncture(enum tetra_punct p, const int8_t *in, unsigned k3,
                      int8_t *mother, unsigned k2);

/** Largest K2 supported by tetra_viterbi(). */
#define TETRA_VITERBI_MAX_K2 292

/**
 * \brief Soft-decision Viterbi decoder for the mother code.
 *
 * The code is assumed to be terminated in state 0 by the 4 tail bits.
 *
 * @param mother: 4 * K2 soft bits (erasures allowed).
 * @param k2: number of type-2 bits to decode, at most TETRA_VITERBI_MAX_K2.
 * @param out: K2 decoded bits.
 * @return 0 on success, -1 if K2 is too large.
 */
int tetra_viterbi(const int8_t *mother, unsigned k2, uint8_t *out);

/**
 * \brief Same as tetra_viterbi() with a caller supplied decision buffer,
 * to keep stack usage low.
 *
 * @param mother: 4 * K2 soft bits (erasures allowed).
 * @param k2: number of type-2 bits to decode.
 * @param out: K2 decoded bits.
 * @param dec: scratch buffer of K2 words.
 * @return 0 on success, -1 if K2 is too large.
 */
int tetra_viterbi_buf(const int8_t *mother, unsigned k2, uint8_t *out,
                      uint16_t *dec);

/**
 * \brief (K, a) block interleaver, EN 300 392-2 clause 8.2.4.1.
 *
 * @param in: K type-3 bits.
 * @param out: K type-4 bits.
 * @param k: block size.
 * @param a: interleaver parameter.
 */
void tetra_interleave(const uint8_t *in, uint8_t *out, unsigned k, unsigned a);

/**
 * \brief Inverse of tetra_interleave() for soft bits.
 *
 * @param in: K soft type-4 bits.
 * @param out: K soft type-3 bits.
 * @param k: block size.
 * @param a: interleaver parameter.
 */
void tetra_deinterleave_soft(const int8_t *in, int8_t *out, unsigned k,
                             unsigned a);

/**
 * \brief Build the 32-bit scrambler state from a 30-bit colour code.
 *
 * @param code: e(1) in bit 29 down to e(30) in bit 0.
 * @return initial scrambler state.
 */
uint32_t tetra_scramb_init(uint32_t code);

/**
 * \brief Scramble or descramble hard bits in place.
 *
 * @param bits: bits to process.
 * @param n: number of bits.
 * @param init: value from tetra_scramb_init().
 */
void tetra_scramble(uint8_t *bits, unsigned n, uint32_t init);

/**
 * \brief Descramble soft bits in place (sign flip where p(k) = 1).
 *
 * @param soft: soft bits to process.
 * @param n: number of bits.
 * @param init: value from tetra_scramb_init().
 */
void tetra_descramble_soft(int8_t *soft, unsigned n, uint32_t init);

/**
 * \brief Scrambling sequence as packed bits.
 *
 * @param init: value from tetra_scramb_init().
 * @param words: output, sequence bit i in bit 31 - i % 32 of words[i / 32].
 * @param n: number of 32-bit words.
 */
void tetra_scramb_words(uint32_t init, uint32_t *words, unsigned n);

/**
 * \brief TMO extended colour code from MCC, MNC and colour code.
 *
 * @param mcc: mobile country code (10 bits).
 * @param mnc: mobile network code (14 bits).
 * @param cc: colour code (6 bits).
 * @return 30-bit code for tetra_scramb_init().
 */
uint32_t tetra_tmo_colour_code(uint16_t mcc, uint16_t mnc, uint8_t cc);

/**
 * \brief DMO colour code from the MNI and the source address.
 *
 * @param mni: 24-bit mobile network identity (only 6 LSBs are used).
 * @param src: 24-bit source address (true or pseudo SSI).
 * @return 30-bit code for tetra_scramb_init().
 */
uint32_t tetra_dmo_colour_code(uint32_t mni, uint32_t src);

/**
 * \brief Encode 14 bits with the shortened (30,14) Reed-Muller code.
 *
 * @param data: 14 data bits, b1(1) in bit 13.
 * @return 30-bit codeword, b2(1) in bit 29.
 */
uint32_t tetra_rm3014_encode(uint16_t data);

/**
 * \brief Decode a (30,14) codeword, correcting up to two bit errors.
 *
 * @param cw: received codeword, b2(1) in bit 29.
 * @param data: decoded 14 data bits.
 * @return number of corrected bits (0..2), or -1 if uncorrectable.
 */
int tetra_rm3014_decode(uint32_t cw, uint16_t *data);

/**
 * \brief Hard decision on soft bits.
 *
 * @param soft: soft bits.
 * @param bits: output bits.
 * @param n: number of bits.
 */
void tetra_soft_to_bits(const int8_t *soft, uint8_t *bits, unsigned n);

/**
 * \brief Pack bits into bytes, MSB first.
 *
 * @param bits: input bits.
 * @param n: number of bits.
 * @param out: output buffer of at least (n + 7) / 8 bytes.
 */
void tetra_pack_bits(const uint8_t *bits, unsigned n, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_CODING_H */
