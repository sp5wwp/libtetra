/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdint>

#include <tetra/tetra_crypto.h>

TEST_CASE("TEA1/2/3 keystreams match Osmocom reference outputs", "[crypto][tea][oracle]")
{
    const std::array<uint8_t, TETRA_TEA_KEY_BYTES> key = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    };
    const std::array<std::array<uint8_t, 32>, 3> expected = {{
        {{0xaa, 0x89, 0x78, 0x4e, 0x83, 0x3f, 0xfa, 0x52,
          0x0a, 0x0e, 0x09, 0xf2, 0xac, 0x1c, 0x4a, 0x34,
          0x63, 0x31, 0x02, 0xb8, 0x14, 0xe6, 0xcf, 0xee,
          0xcd, 0xcf, 0x37, 0x76, 0x0d, 0x90, 0x09, 0xdb}},
        {{0x67, 0x74, 0x21, 0x8f, 0xe2, 0x6d, 0xfc, 0xb9,
          0x40, 0x65, 0x8d, 0xea, 0x18, 0xb0, 0x7f, 0xe5,
          0xfa, 0xad, 0x56, 0x73, 0x9f, 0x07, 0x1e, 0xc8,
          0x04, 0x28, 0xd7, 0x18, 0x29, 0x53, 0x0b, 0x41}},
        {{0xaf, 0x1a, 0x6a, 0xc8, 0xaf, 0x3e, 0x52, 0x28,
          0x39, 0x63, 0x79, 0x8e, 0xd0, 0x26, 0xd9, 0x51,
          0x74, 0x34, 0x39, 0xd9, 0x57, 0x3e, 0x52, 0x69,
          0x59, 0x28, 0x42, 0xab, 0xc4, 0x9f, 0x27, 0x3d}},
    }};

    std::array<uint8_t, 32> output{};
    for (unsigned algorithm = TETRA_TEA1; algorithm <= TETRA_TEA3;
         algorithm++) {
        REQUIRE(tetra_tea_generate(static_cast<tetra_tea_algorithm>(algorithm),
                                   0, key.data(), output.size(), output.data()));
        REQUIRE(output == expected[algorithm - TETRA_TEA1]);
    }
}


TEST_CASE("TB5 derives the 80-bit ECK from network parameters", "[crypto][tb5]")
{
    const std::array<uint8_t, TETRA_TEA_KEY_BYTES> ck = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    };
    const std::array<uint8_t, TETRA_TEA_KEY_BYTES> expected = {
        0x48, 0xd3, 0xad, 0x2e, 0xaf, 0xce, 0x6c, 0xf5, 0xd2, 0xb5,
    };
    std::array<uint8_t, TETRA_TEA_KEY_BYTES> eck{};

    REQUIRE(tetra_tb5(0xabc, 0x1234, 0x2d, ck.data(), eck.data()));
    REQUIRE(eck == expected);
    REQUIRE_FALSE(tetra_tb5(0x1000, 0, 0, ck.data(), eck.data()));
    REQUIRE_FALSE(tetra_tb5(0, 0x4000, 0, ck.data(), eck.data()));
    REQUIRE_FALSE(tetra_tb5(0, 0, 0x40, ck.data(), eck.data()));
}

TEST_CASE("TEA IV changes the generated keystream", "[crypto][tea][iv][oracle]")
{
    const std::array<uint8_t, TETRA_TEA_KEY_BYTES> key = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    };
    const std::array<std::array<uint8_t, 8>, 3> expected = {{
        {{0x71, 0xcc, 0xb6, 0x79, 0x9d, 0xf7, 0xb6, 0x98}},
        {{0x51, 0x99, 0x89, 0x70, 0x86, 0x65, 0x65, 0x37}},
        {{0x7b, 0xfc, 0x52, 0xa3, 0xcd, 0xa8, 0x2f, 0x30}},
    }};
    std::array<uint8_t, 8> output{};

    for (unsigned algorithm = TETRA_TEA1; algorithm <= TETRA_TEA3;
         algorithm++) {
        REQUIRE(tetra_tea_generate(static_cast<tetra_tea_algorithm>(algorithm),
                                   0x01234567u, key.data(), output.size(),
                                   output.data()));
        REQUIRE(output == expected[algorithm - TETRA_TEA1]);
    }
}

TEST_CASE("TMO cipher IV follows the 29-bit ETSI field layout", "[crypto][tea][iv]")
{
    const tetra_time time = { 4, 18, 60 };
    uint32_t iv = 0;

    REQUIRE(tetra_tea_build_iv(&time, 0xffff, TETRA_TEA_UPLINK, &iv));
    REQUIRE(iv == 0x1ffffe4bu);

    REQUIRE(tetra_tea_build_iv(&time, 0xffff, TETRA_TEA_DOWNLINK, &iv));
    REQUIRE(iv == 0x0ffffe4bu);
}

TEST_CASE("TEA API rejects invalid parameters", "[crypto][tea]")
{
    const uint8_t key[TETRA_TEA_KEY_BYTES] = { 0 };
    uint8_t output[1] = {};
    uint32_t iv;
    tetra_time time = { 1, 1, 1 };

    REQUIRE_FALSE(tetra_tea_generate(static_cast<tetra_tea_algorithm>(0),
                                     0, key, sizeof(output), output));
    REQUIRE_FALSE(tetra_tea_generate(TETRA_TEA1, 0x20000000u, key,
                                     sizeof(output), output));
    REQUIRE_FALSE(tetra_tea_generate(TETRA_TEA1, 0, nullptr,
                                     sizeof(output), output));
    REQUIRE_FALSE(tetra_tea_generate(TETRA_TEA1, 0, key, sizeof(output),
                                     nullptr));

    time.tn = 0;
    REQUIRE_FALSE(tetra_tea_build_iv(&time, 0, TETRA_TEA_DOWNLINK, &iv));
    time.tn = 1;
    time.mn = 0;
    REQUIRE_FALSE(tetra_tea_build_iv(&time, 0, TETRA_TEA_DOWNLINK, &iv));
    time.mn = 1;
    REQUIRE_FALSE(tetra_tea_build_iv(&time, 0,
                                     static_cast<tetra_tea_direction>(2), &iv));
}

TEST_CASE("TA61 recovers the SSI from the encrypted short identity",
          "[crypto][ta61][oracle]")
{
    /* Oracle vector produced with osmo-tetra's ta61(): CCK below encrypts
     * SSI 0x0a1b2c to ESI 0xc40a49. */
    const uint8_t cck[TETRA_TEA_KEY_BYTES] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
    };
    uint32_t ssi = 0;

    REQUIRE(tetra_ta61_decrypt_ssi(cck, 0xc40a49u, &ssi));
    REQUIRE(ssi == 0x0a1b2cu);

    /* A different CCK does not recover the same identity. */
    uint8_t other[TETRA_TEA_KEY_BYTES];
    for (unsigned i = 0; i < TETRA_TEA_KEY_BYTES; i++)
        other[i] = (uint8_t)(cck[i] ^ 1u);
    REQUIRE(tetra_ta61_decrypt_ssi(other, 0xc40a49u, &ssi));
    REQUIRE(ssi != 0x0a1b2cu);

    REQUIRE_FALSE(tetra_ta61_decrypt_ssi(cck, 0xc40a49u, nullptr));
    REQUIRE_FALSE(tetra_ta61_decrypt_ssi(nullptr, 0xc40a49u, &ssi));
}
