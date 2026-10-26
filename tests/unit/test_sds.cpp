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

namespace
{

int lip(const writer &w, struct tetra_lip *l)
{
    struct tetra_sds s;

    std::memset(&s, 0, sizeof(s));
    s.type = TETRA_SDS_UDD4;
    s.data = w.b.data();
    s.nbits = (uint16_t)w.n;
    return tetra_sds_parse_lip(&s, l);
}

} // namespace

TEST_CASE("LIP short location report", "[sds][lip]")
{
    writer w;
    struct tetra_lip l;

    w.put(TETRA_PID_LIP, 8);
    w.put(0, 2);        /* SHORT LOCATION REPORT */
    w.put(1, 2);        /* less than 5 min */
    w.put(1958479, 25); /* 21.0122 E */
    w.put(4868161, 24); /* 52.2297 N */
    w.put(1, 3);        /* less than 20 m */
    w.put(67, 7);       /* 120 km/h */
    w.put(4, 4);        /* east */
    w.put(0, 1);        /* reason for sending */
    w.put(3, 8);        /* push-to-talk */
    REQUIRE(lip(w, &l) == 0);
    REQUIRE(l.kind == TETRA_LIP_SHORT_REPORT);
    REQUIRE(l.time_elapsed == 1);
    REQUIRE(l.has_position);
    REQUIRE(l.lat_e7 == 522297013);
    REQUIRE(l.lon_e7 == 210122001);
    REQUIRE(l.position_error == 1);
    REQUIRE(l.speed_dkmh == 1199);
    REQUIRE(l.direction_ddeg == 900);
    REQUIRE(l.reason == 3);
    REQUIRE(l.user_data == 0xffff);
    REQUIRE(l.vspeed_dkmh == INT16_MIN);
}

TEST_CASE("LIP long location report with shape, velocity and options",
          "[sds][lip]")
{
    writer w;
    struct tetra_lip l;

    w.put(TETRA_PID_LIP, 8);
    w.put(1, 2);
    w.put(3, 4); /* LONG LOCATION REPORT */
    w.put(2, 2); /* time of position */
    w.put(26, 5);
    w.put(14, 5);
    w.put(30, 6);
    w.put(5, 6);
    w.put(TETRA_LIP_CIRCLE_ALT_UNC, 4);
    w.put(14094288, 25);            /* 151.2153 E */
    w.put(-3155682 & 0xffffff, 24); /* 33.8568 S */
    w.put(10, 6);                   /* less than 27 m */
    w.put(0, 1);                    /* WGS84 */
    w.put(1500, 11);                /* 1598 m */
    w.put(2, 3);                    /* altitude: less than 5 m */
    w.put(7, 3);                    /* velocity: all fields */
    w.put(40, 7);                   /* 43.8 km/h */
    w.put(1, 3);
    w.put(1, 1); /* downwards */
    w.put(5, 7); /* 5 km/h */
    w.put(2, 3);
    w.put(200, 8); /* 281.25 degrees */
    w.put(3, 3);
    w.put(1, 1); /* acknowledgement requested */
    w.put(1, 1); /* user defined data */
    w.put(0x5a, 8);
    w.put(6, 5); /* location message reference */
    w.put(8, 6);
    w.put(77, 8);
    w.put(1, 5); /* extended user data: 9 octets */
    w.put(0, 6);
    w.put(2, 7);
    w.put(0, 72);
    w.put(17, 5); /* terminal identification */
    w.put(4 + 48, 6);
    w.put(2, 4); /* SSI and MNI */
    w.put(1234567, 24);
    w.put(0x3abcde, 24);
    REQUIRE(lip(w, &l) == 0);
    REQUIRE(l.kind == TETRA_LIP_LONG_REPORT);
    REQUIRE(l.has_time_of_position);
    REQUIRE(l.day == 26);
    REQUIRE(l.hour == 14);
    REQUIRE(l.minute == 30);
    REQUIRE(l.second == 5);
    REQUIRE(l.time_elapsed == TETRA_LIP_NONE);
    REQUIRE(l.shape == TETRA_LIP_CIRCLE_ALT_UNC);
    REQUIRE(l.lat_e7 == -338567948);
    REQUIRE(l.lon_e7 == 1512153053);
    REQUIRE(l.h_uncertainty == 10);
    REQUIRE(tetra_lip_uncertainty_m(l.h_uncertainty) == 27);
    REQUIRE(l.has_altitude);
    REQUIRE_FALSE(l.altitude_user);
    REQUIRE(l.altitude_m == 1598);
    REQUIRE(l.altitude_uncertainty == 2);
    REQUIRE(l.speed_dkmh == 438);
    REQUIRE(l.speed_uncertainty == 1);
    REQUIRE(l.vspeed_dkmh == -50);
    REQUIRE(l.vspeed_uncertainty == 2);
    REQUIRE(l.direction_ddeg == 2813);
    REQUIRE(l.direction_uncertainty == 3);
    REQUIRE(l.ack_request);
    REQUIRE(l.user_data == 0x5a);
    REQUIRE(l.reason == 0xffff);
    REQUIRE(l.msg_ref == 77);
    REQUIRE(l.id_type == 2);
    REQUIRE(l.id_ssi == 1234567);
    REQUIRE(l.id_mni == 0x3abcde);
    REQUIRE_FALSE(l.has_status);
}

TEST_CASE("LIP other PDUs, unknown shapes and other protocols", "[sds][lip]")
{
    struct tetra_lip l;

    SECTION("location reporting enable/disable request")
    {
        writer w;

        w.put(TETRA_PID_LIP, 8);
        w.put(1, 2);
        w.put(10, 4);
        w.put(0, 9);
        REQUIRE(lip(w, &l) == 0);
        REQUIRE(l.kind == TETRA_LIP_OTHER);
        REQUIRE(l.pdu_ext == 10);
    }

    SECTION("location shape extension stops decoding")
    {
        writer w;

        w.put(TETRA_PID_LIP, 8);
        w.put(1, 2);
        w.put(3, 4);
        w.put(0, 2);
        w.put(15, 4);
        w.put(0, 40);
        REQUIRE(lip(w, &l) == 0);
        REQUIRE(l.kind == TETRA_LIP_LONG_REPORT);
        REQUIRE_FALSE(l.has_position);
        REQUIRE(l.speed_dkmh == 0xffff);
    }

    SECTION("truncated short report")
    {
        writer w;

        w.put(TETRA_PID_LIP, 8);
        w.put(0, 2);
        w.put(0, 30);
        REQUIRE(lip(w, &l) == -1);
    }

    SECTION("text is not LIP")
    {
        writer w;

        w.put(TETRA_PID_SIMPLE_TEXT, 8);
        w.put(0, 80);
        REQUIRE(lip(w, &l) == -1);
    }
}

TEST_CASE("LIP position uncertainty in metres", "[sds][lip]")
{
    REQUIRE(tetra_lip_uncertainty_m(0) == 1);
    REQUIRE(tetra_lip_uncertainty_m(20) == 187);
    REQUIRE(tetra_lip_uncertainty_m(40) == 7311);
    REQUIRE(tetra_lip_uncertainty_m(62) == 403803);
    REQUIRE(tetra_lip_uncertainty_m(63) == 0);
}
