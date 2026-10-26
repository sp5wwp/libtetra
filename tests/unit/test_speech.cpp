/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <vector>

#include <tetra/tetra_speech.h>
#include <tetra/tetra_tx.h>

#include "../vectors/osmo_tmo_tch.h"

namespace
{

using bits_t = std::vector<uint8_t>;

bits_t random_bits(unsigned n, uint32_t seed)
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

std::vector<int8_t> to_soft(const uint8_t *b, unsigned n)
{
    std::vector<int8_t> s(n);

    for (unsigned i = 0; i < n; i++)
        s[i] = b[i] ? 64 : -64;
    return s;
}

/* Type-4 position of type-3 bit m in a full slot (clause 5.5.3). */
unsigned full_t4(unsigned m)
{
    return (m % 18) * 24 + m / 18;
}

struct tetra_speech_ws ws;

} // namespace

TEST_CASE("Speech slot encode and decode round trip", "[speech]")
{
    for (uint32_t seed = 1; seed < 20; seed++) {
        bits_t a = random_bits(137, seed), b = random_bits(137, seed + 100);
        uint8_t t4[432], da[137], db[137];

        tetra_speech_encode_slot(a.data(), b.data(), t4);
        std::vector<int8_t> soft = to_soft(t4, 432);

        /* errors in the convolutionally coded classes are corrected */
        for (unsigned m = 102 + 7; m < 432; m += 41)
            soft[full_t4(m)] = (int8_t)-soft[full_t4(m)];

        REQUIRE(tetra_speech_decode_slot(&ws, soft.data(), da, db));
        REQUIRE(std::memcmp(da, a.data(), 137) == 0);
        REQUIRE(std::memcmp(db, b.data(), 137) == 0);
    }
}

TEST_CASE("Half-slot speech encode and decode round trip", "[speech]")
{
    for (uint32_t seed = 1; seed < 20; seed++) {
        bits_t a = random_bits(137, seed * 7);
        uint8_t t4[216], da[137];

        tetra_speech_encode_half(a.data(), t4);
        std::vector<int8_t> soft = to_soft(t4, 216);

        REQUIRE(tetra_speech_decode_half(&ws, soft.data(), da));
        REQUIRE(std::memcmp(da, a.data(), 137) == 0);
    }
}

TEST_CASE("Speech CRC flags corrupted class 2 bits", "[speech]")
{
    bits_t a = random_bits(137, 3), b = random_bits(137, 4);
    uint8_t t4[432], da[137], db[137];

    tetra_speech_encode_slot(a.data(), b.data(), t4);
    std::vector<int8_t> soft = to_soft(t4, 432);

    /* wipe out the class 2 part: type-3 bits 270..431 */
    for (unsigned m = 270; m < 432; m += 2)
        soft[full_t4(m)] = (int8_t)-soft[full_t4(m)];
    REQUIRE_FALSE(tetra_speech_decode_slot(&ws, soft.data(), da, db));
}

TEST_CASE("Speech frames of a stolen slot", "[speech]")
{
    bits_t b = random_bits(137, 9);
    int8_t slot[432];
    uint8_t t4[216];
    struct tetra_traffic t;
    struct tetra_speech sp;

    tetra_speech_encode_half(b.data(), t4);
    std::memset(slot, 0, sizeof(slot));
    for (unsigned i = 0; i < 216; i++)
        slot[216 + i] = t4[i] ? 64 : -64;

    std::memset(&t, 0, sizeof(t));
    t.half[0] = TETRA_HALF_STOLEN_C;
    t.half[1] = TETRA_HALF_TCH;
    t.bits = slot;
    REQUIRE(tetra_speech_decode(&ws, &t, &sp) == 1);
    REQUIRE(sp.bfi[0]);
    REQUIRE_FALSE(sp.bfi[1]);
    REQUIRE(std::memcmp(sp.bits[1], b.data(), 137) == 0);

    /* circuit mode data is not speech */
    t.circuit_type = 1;
    REQUIRE(tetra_speech_decode(&ws, &t, &sp) == 0);
    REQUIRE(sp.bfi[0]);
    REQUIRE(sp.bfi[1]);
}

TEST_CASE("Speech decoding of real TMO traffic passes the CRC",
          "[speech][oracle]")
{
    for (unsigned f = 0; f < 5; f++) {
        int8_t soft[432];
        struct tetra_traffic t;
        struct tetra_speech sp;

        for (unsigned i = 0; i < 432; i++)
            soft[i] =
                (osmo_tmo_call_tch[f][i / 8] >> (7 - i % 8)) & 1 ? 64 : -64;
        std::memset(&t, 0, sizeof(t));
        t.bits = soft;
        REQUIRE(tetra_speech_decode(&ws, &t, &sp) == 2);
        REQUIRE_FALSE(sp.bfi[0]);
        REQUIRE_FALSE(sp.bfi[1]);
    }
}
