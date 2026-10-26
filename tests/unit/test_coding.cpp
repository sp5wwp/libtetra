/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <vector>

#include <tetra/tetra_coding.h>
#include <tetra/tetra_tx.h>

extern "C" {
#include "l2/lmac.h"
}

#include "../vectors/osmo_vectors.h"

using bits_t = std::vector<uint8_t>;

static bits_t unpack(const uint8_t *packed, unsigned n)
{
    bits_t b(n);

    for (unsigned i = 0; i < n; i++)
        b[i] = (packed[i / 8] >> (7 - i % 8)) & 1;
    return b;
}

static bits_t random_bits(unsigned n, uint32_t seed)
{
    bits_t b(n);

    for (unsigned i = 0; i < n; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        b[i] = seed & 1;
    }
    return b;
}

static std::vector<int8_t> to_soft(const bits_t &b)
{
    std::vector<int8_t> s(b.size());

    for (size_t i = 0; i < b.size(); i++)
        s[i] = b[i] ? TETRA_SOFT_ONE : TETRA_SOFT_ZERO;
    return s;
}

static uint32_t cell_scramb()
{
    return tetra_scramb_init(tetra_tmo_colour_code(235, 78, 1));
}

TEST_CASE("CRC-16 append and check", "[coding][crc]")
{
    bits_t b = random_bits(268 + 16, 7);

    tetra_crc16_append(b.data(), 268);
    REQUIRE(tetra_crc16_check(b.data(), 268));
    for (unsigned i = 0; i < 268 + 16; i += 37) {
        b[i] ^= 1;
        REQUIRE_FALSE(tetra_crc16_check(b.data(), 268));
        b[i] ^= 1;
    }
}

TEST_CASE("FCS is the non-reflected CRC-32", "[coding][fcs]")
{
    const char *msg = "123456789";
    bits_t b;

    for (const char *p = msg; *p; p++) {
        for (int k = 7; k >= 0; k--)
            b.push_back((*p >> k) & 1);
    }
    REQUIRE(tetra_fcs32(b.data(), (unsigned)b.size()) == 0xfc891918u);

    uint8_t packed[9];

    std::memcpy(packed, msg, 9);
    REQUIRE(tetra_fcs32_packed(packed, 0, 72) == 0xfc891918u);
}

TEST_CASE("Scrambling sequence matches osmo-tetra", "[coding][scramb][oracle]")
{
    bits_t seq(64, 0);

    tetra_scramble(seq.data(), 64, cell_scramb());
    REQUIRE(seq == unpack(osmo_scramb_seq, 64));
}

TEST_CASE("RM(30,14) encoding matches osmo-tetra", "[coding][rm][oracle]")
{
    for (unsigned i = 0; i < 8; i++)
        REQUIRE(tetra_rm3014_encode(osmo_rm_in[i]) == osmo_rm_out[i]);
}

TEST_CASE("RM(30,14) corrects up to two errors", "[coding][rm]")
{
    for (unsigned d = 0; d < 0x4000; d += 97) {
        uint32_t cw = tetra_rm3014_encode((uint16_t)d);
        uint16_t out;

        REQUIRE(tetra_rm3014_decode(cw, &out) == 0);
        REQUIRE(out == d);
        for (unsigned i = 0; i < 30; i++) {
            REQUIRE(tetra_rm3014_decode(cw ^ (1u << i), &out) == 1);
            REQUIRE(out == d);
            unsigned j = (i + 7 + d) % 30;

            if (j != i) {
                REQUIRE(tetra_rm3014_decode(cw ^ (1u << i) ^ (1u << j), &out) ==
                        2);
                REQUIRE(out == d);
            }
        }
    }
}

TEST_CASE("Block interleaver round trip", "[coding][interleave]")
{
    const unsigned k[3] = { 120, 216, 432 }, a[3] = { 11, 101, 103 };

    for (unsigned c = 0; c < 3; c++) {
        bits_t in = random_bits(k[c], 3 + c), out(k[c]);
        std::vector<int8_t> soft(k[c]), back(k[c]);

        tetra_interleave(in.data(), out.data(), k[c], a[c]);
        soft = to_soft(out);
        tetra_deinterleave_soft(soft.data(), back.data(), k[c], a[c]);
        for (unsigned i = 0; i < k[c]; i++)
            REQUIRE((back[i] > 0) == (in[i] != 0));
    }
}

TEST_CASE("RCPC rates 292/432 and 148/432 match osmo-tetra",
          "[coding][rcpc][oracle]")
{
    struct {
        enum tetra_punct p;
        unsigned k2;
        const uint8_t *t2, *t3;
    } v[2] = {
        { TETRA_PUNCT_292_432, 292, osmo_tch48_type2, osmo_tch48_type3 },
        { TETRA_PUNCT_148_432, 148, osmo_tch24_type2, osmo_tch24_type3 },
    };

    for (auto &c : v) {
        bits_t t2 = unpack(c.t2, c.k2), mother(4 * c.k2), t3(432);
        std::vector<int8_t> soft, m(4 * c.k2);
        bits_t dec(c.k2);

        tetra_conv_encode(t2.data(), c.k2, mother.data());
        tetra_puncture(c.p, mother.data(), t3.data(), 432);
        REQUIRE(t3 == unpack(c.t3, 432));

        soft = to_soft(t3);
        tetra_depuncture(c.p, soft.data(), 432, m.data(), c.k2);
        REQUIRE(tetra_viterbi(m.data(), c.k2, dec.data()) == 0);
        REQUIRE(dec == t2);
    }
}

TEST_CASE("Signalling channel encoding matches osmo-tetra",
          "[coding][lmac][oracle]")
{
    struct {
        enum tetra_sch ch;
        unsigned k1, k5;
        const uint8_t *t1, *t5;
        uint32_t scramb;
    } v[4] = {
        { TETRA_SCH_S, 60, 120, osmo_sch_s_type1, osmo_sch_s_type5,
          cell_scramb() },
        { TETRA_SCH_H, 124, 216, osmo_sch_h_type1, osmo_sch_h_type5,
          cell_scramb() },
        { TETRA_SCH_F, 268, 432, osmo_sch_f_type1, osmo_sch_f_type5,
          cell_scramb() },
        { TETRA_SCH_S, 60, 120, osmo_bsch_type1, osmo_bsch_type5,
          tetra_scramb_init(0) },
    };

    for (auto &c : v) {
        bits_t t1 = unpack(c.t1, c.k1), t5(c.k5);

        tetra_encode_sch(c.ch, t1.data(), c.scramb, t5.data());
        REQUIRE(t5 == unpack(c.t5, c.k5));
    }
}

TEST_CASE("Signalling channel decoding with errors", "[coding][lmac]")
{
    static struct tetra_lmac_ws ws;
    const unsigned k1[3] = { 60, 124, 268 }, k5[3] = { 120, 216, 432 };

    for (unsigned c = 0; c < 3; c++) {
        bits_t t1 = random_bits(k1[c], 11 + c), t5(k5[c]);
        uint8_t out[40];

        tetra_encode_sch((enum tetra_sch)c, t1.data(), cell_scramb(),
                         t5.data());
        std::vector<int8_t> soft = to_soft(t5);

        /* 2 % hard errors, spread out (the rate 2/3 code is weak on
         * hard decisions: the receiver normally gets soft bits) */
        for (unsigned i = 5; i < k5[c]; i += 50)
            soft[i] = (int8_t)-soft[i];
        REQUIRE(lmac_decode(&ws, (enum lmac_chan)c, soft.data(), cell_scramb(),
                            out));
        REQUIRE(unpack(out, k1[c]) == t1);

        /* wrong colour code must fail the CRC */
        REQUIRE_FALSE(lmac_decode(&ws, (enum lmac_chan)c, soft.data(),
                                  tetra_scramb_init(1), out));
    }
}
