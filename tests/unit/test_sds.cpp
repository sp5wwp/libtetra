/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

#include <tetra/tetra_sds.h>

namespace
{

struct writer {
    std::vector<uint8_t> b = std::vector<uint8_t>(300, 0);
    unsigned n = 0;

    void put(uint32_t v, unsigned k)
    {
        for (unsigned i = 0; i < k; i++, n++) {
            if ((v >> (k - 1 - i)) & 1)
                b[n / 8] |= (uint8_t)(0x80 >> (n % 8));
        }
    }
};

std::string decode(const writer &w)
{
    struct tetra_sds s;
    struct tetra_sds_text t;
    char out[256];

    std::memset(&s, 0, sizeof(s));
    s.type = TETRA_SDS_UDD4;
    s.data = w.b.data();
    s.nbits = (uint16_t)w.n;
    if (tetra_sds_parse_text(&s, &t) != 0)
        return "<not text>";
    REQUIRE(tetra_sds_text_utf8(&t, out, sizeof(out)) >= 0);
    return out;
}

} // namespace

TEST_CASE("Simple text messaging, ISO 8859-1", "[sds]")
{
    writer w;
    const char *txt = "Gr\xfc\xdf Gott";

    w.put(TETRA_PID_SIMPLE_TEXT, 8);
    w.put(0, 1);
    w.put(TETRA_TCS_LATIN1, 7);
    for (const char *p = txt; *p; p++)
        w.put((uint8_t)*p, 8);
    REQUIRE(decode(w) == "Gr\xc3\xbc\xc3\x9f Gott");
}

TEST_CASE("SDS-TL text with forward address and time stamp", "[sds]")
{
    writer w;

    w.put(TETRA_PID_TEXT, 8);
    w.put(0, 4); /* SDS-TRANSFER */
    w.put(1, 2); /* receipt report */
    w.put(0, 1);
    w.put(1, 1); /* storage / forward control present */
    w.put(99, 8);
    w.put(3, 5); /* validity */
    w.put(1, 3); /* forward address: SSI */
    w.put(123456, 24);
    w.put(1, 1); /* time stamp used */
    w.put(TETRA_TCS_LATIN1, 7);
    w.put(0x123456, 24);
    w.put('O', 8);
    w.put('K', 8);
    REQUIRE(decode(w) == "OK");
}

TEST_CASE("GSM 7-bit text with escape and padding", "[sds]")
{
    /* "@{x}" plus 4 more chars: 8 septets = 7 octets exactly */
    const uint8_t sept[8] = { 0x00, 0x1b, 0x28, 0x78, 0x1b, 0x29, 0x61, 0x00 };
    uint8_t oct[7] = { 0 };
    writer w;

    for (unsigned i = 0; i < 8; i++) {
        unsigned bit = 7 * i;

        oct[bit / 8] |= (uint8_t)(sept[i] << (bit % 8));
        if (bit % 8 > 1 && bit / 8 + 1 < 7)
            oct[bit / 8 + 1] |= (uint8_t)(sept[i] >> (8 - bit % 8));
    }
    w.put(TETRA_PID_SIMPLE_TEXT, 8);
    w.put(0, 1);
    w.put(TETRA_TCS_GSM7, 7);
    for (unsigned i = 0; i < 7; i++)
        w.put(oct[i], 8);
    /* trailing '@' in the last septet is treated as padding */
    REQUIRE(decode(w) == "@{x}a");
}

TEST_CASE("UCS-2 text with a surrogate pair", "[sds]")
{
    writer w;

    w.put(TETRA_PID_SIMPLE_TEXT, 8);
    w.put(0, 1);
    w.put(TETRA_TCS_UCS2, 7);
    w.put(0x03a9, 16); /* Omega */
    w.put(0xd83d, 16); /* U+1F4FB radio */
    w.put(0xdcfb, 16);
    REQUIRE(decode(w) == "\xce\xa9\xf0\x9f\x93\xbb");
}

TEST_CASE("Non-text protocol identifiers are rejected", "[sds]")
{
    writer w;

    w.put(0x03, 8); /* simple location system */
    w.put(0, 16);
    REQUIRE(decode(w) == "<not text>");
}

namespace
{

std::string location(const writer &w, struct tetra_sds_location *l)
{
    struct tetra_sds s;
    char out[256];

    std::memset(&s, 0, sizeof(s));
    s.type = TETRA_SDS_UDD4;
    s.data = w.b.data();
    s.nbits = (uint16_t)w.n;
    if (tetra_sds_parse_location(&s, l) != 0)
        return "<not location>";
    REQUIRE(tetra_sds_location_ascii(l, out, sizeof(out)) >= 0);
    return out;
}

} // namespace

TEST_CASE("Location system messages carry NMEA sentences", "[sds]")
{
    const char *nmea = "$GPGGA,123519,4807.038,N,01131.000,E,1,08*47\r\n";
    struct tetra_sds_location l;

    SECTION("simple location system")
    {
        writer w;

        w.put(TETRA_PID_SIMPLE_LOCATION, 8);
        w.put(TETRA_LCS_NMEA, 8);
        for (const char *p = nmea; *p; p++)
            w.put((uint8_t)*p, 8);
        REQUIRE(location(w, &l) == nmea);
        REQUIRE_FALSE(l.sdstl);
        REQUIRE(l.coding == TETRA_LCS_NMEA);
    }

    SECTION("location system over SDS-TL")
    {
        writer w;

        w.put(TETRA_PID_LOCATION, 8);
        w.put(0, 4); /* SDS-TRANSFER */
        w.put(1, 2); /* delivery report request */
        w.put(0, 2);
        w.put(42, 8); /* message reference */
        w.put(TETRA_LCS_NMEA, 8);
        for (const char *p = nmea; *p; p++)
            w.put((uint8_t)*p, 8);
        REQUIRE(location(w, &l) == nmea);
        REQUIRE(l.sdstl);
        REQUIRE(l.msg_ref == 42);
        REQUIRE(l.report_req == 1);
    }

    SECTION("text is not location")
    {
        writer w;

        w.put(TETRA_PID_SIMPLE_TEXT, 8);
        w.put(0, 8);
        REQUIRE(location(w, &l) == "<not location>");
    }
}
