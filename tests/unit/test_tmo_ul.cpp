/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

#include <tetra/tetra.h>
#include <tetra/tetra_coding.h>
#include <tetra/tetra_tx.h>

extern "C" {
#include "util/bits.h"
}

TEST_CASE("Uplink frequency follows the duplex spacing table", "[tmo][ul]")
{
    /* 400 MHz band, duplex code 0: 10 MHz below, or above if reversed */
    REQUIRE(tetra_uplink_hz(392512500u, 3, 0, false) == 382512500u);
    REQUIRE(tetra_uplink_hz(392512500u, 3, 0, true) == 402512500u);
    /* 800 MHz band, code 1: 45 MHz */
    REQUIRE(tetra_uplink_hz(851000000u, 8, 1, false) == 806000000u);
    /* Reserved spacing and unknown frequency */
    REQUIRE(tetra_uplink_hz(392512500u, 3, 6, false) == 0);
    REQUIRE(tetra_uplink_hz(0, 3, 0, false) == 0);
}

#if TETRA_UPLINK_DECODE

namespace
{

const uint16_t MCC = 901, MNC = 9999;
const uint8_t CC = 5;

struct ul_capture {
    struct tetra_tmo_rx rx;
    std::vector<struct tetra_call> calls;
    std::vector<struct tetra_sds> sds;
    std::vector<std::string> texts;
    std::vector<struct tetra_traffic> traffic;
    std::vector<std::vector<uint8_t>> tch; /* hard type-4 bits */
    std::vector<struct tetra_pdu> pdus;
    unsigned events = 0, uplink_events = 0, bad_time = 0;
};

void on_event(void *user, const struct tetra_event *ev)
{
    ul_capture *c = static_cast<ul_capture *>(user);

    c->events++;
    if (ev->uplink) {
        c->uplink_events++;
        if (ev->time.tn || ev->time.fn || ev->time.mn)
            c->bad_time++;
    }
    switch (ev->type) {
    case TETRA_EV_CALL:
        c->calls.push_back(ev->u.call);
        break;
    case TETRA_EV_SDS: {
        struct tetra_sds_text t;
        char buf[256];

        c->sds.push_back(ev->u.sds);
        if (tetra_sds_parse_text(&ev->u.sds, &t) == 0 &&
            tetra_sds_text_utf8(&t, buf, sizeof(buf)) >= 0)
            c->texts.push_back(buf);
        break;
    }
    case TETRA_EV_TRAFFIC: {
        std::vector<uint8_t> b;

        for (unsigned i = 0; i < TETRA_TCH_BITS; i++)
            b.push_back(ev->u.traffic.bits[i] > 0);
        c->traffic.push_back(ev->u.traffic);
        c->tch.push_back(b);
        break;
    }
    case TETRA_EV_PDU:
        c->pdus.push_back(ev->u.pdu);
        break;
    default:
        break;
    }
}

void init(ul_capture &c, bool set_cell = true)
{
    struct tetra_rx_config cfg;

    std::memset(&cfg, 0, sizeof(cfg));
    cfg.cb = on_event;
    cfg.user = &c;
    cfg.promiscuous = true;
    tetra_tmo_init(&c.rx, &cfg);
    if (set_cell)
        tetra_tmo_set_uplink_cell(&c.rx, MCC, MNC, CC);
}

uint32_t scramb()
{
    return tetra_scramb_init(tetra_tmo_colour_code(MCC, MNC, CC));
}

struct pdu {
    uint8_t b[400] = { 0 };
    unsigned n = 0;
    void put(uint32_t v, unsigned k)
    {
        bw_put(b, &n, v, k);
    }
    void put_bits(const pdu &src, unsigned from, unsigned k)
    {
        bits_copy(b, n, src.b, from, k);
        n += k;
    }
    void fill_to(unsigned end)
    {
        if (n >= end)
            return;
        put(1, 1);
        while (n < end)
            put(0, 1);
    }
    std::vector<uint8_t> bits(unsigned k) const
    {
        std::vector<uint8_t> v(k);

        for (unsigned i = 0; i < k; i++)
            v[i] = (uint8_t)bit_at(b, i);
        return v;
    }
};

/* BL-UDATA, MLE: CMCE, U-TX DEMAND. */
pdu u_tx_demand(uint16_t call_id)
{
    pdu p;

    p.put(2, 4);
    p.put(2, 3);
    p.put(10, 5);
    p.put(call_id, 14);
    p.put(1, 2); /* TX demand priority */
    p.put(0, 1); /* encryption control */
    p.put(0, 1);
    p.put(0, 1); /* O-bit */
    return p;
}

/* BL-UDATA, MLE: CMCE, U-SETUP to a group. */
pdu u_setup(uint32_t gssi)
{
    pdu p;

    p.put(2, 4);
    p.put(2, 3);
    p.put(7, 5);
    p.put(0, 4 + 1 + 1);
    p.put(1 << 2, 8); /* speech, clear, point to multipoint */
    p.put(1, 1);      /* request to transmit */
    p.put(5, 4);      /* priority */
    p.put(0, 2);
    p.put(1, 2); /* called party SSI */
    p.put(gssi, 24);
    p.put(0, 1);
    return p;
}

/* BL-UDATA, MLE: CMCE, U-SDS DATA with an SDS-TL text (GSM 7-bit). */
pdu u_sds_text(uint32_t dst, const char *text)
{
    pdu p;
    unsigned len = (unsigned)std::strlen(text);
    unsigned octets = (7 * len + 7) / 8;
    uint8_t packed[200] = { 0 };

    for (unsigned i = 0; i < len; i++) {
        unsigned v = (unsigned)text[i] & 0x7f, bit = 7 * i;

        packed[bit / 8] |= (uint8_t)(v << (bit % 8));
        if (bit % 8 > 1)
            packed[bit / 8 + 1] |= (uint8_t)(v >> (8 - bit % 8));
    }
    p.put(2, 4);
    p.put(2, 3);
    p.put(15, 5); /* U-SDS-DATA */
    p.put(0, 4);  /* area selection */
    p.put(1, 2);  /* called party SSI */
    p.put(dst, 24);
    p.put(3, 2); /* UDD4 */
    p.put(8 + 4 + 2 + 1 + 1 + 8 + 1 + 7 + 8 * octets, 11);
    p.put(0x82, 8);
    p.put(0, 4 + 2 + 1 + 1);
    p.put(9, 8);
    p.put(0, 1);
    p.put(0, 7);
    for (unsigned i = 0; i < octets; i++)
        p.put(packed[i], 8);
    p.put(0, 1);
    return p;
}

/* MAC-ACCESS with SSI, no optional field: SDU then fill bits to 92. */
std::vector<uint8_t> mac_access(uint32_t ssi, const pdu &sdu)
{
    pdu m;

    m.put(0, 1);
    m.put(1, 1); /* fill bits */
    m.put(0, 1);
    m.put(0, 2); /* SSI */
    m.put(ssi, 24);
    m.put(0, 1);
    m.put_bits(sdu, 0, sdu.n);
    REQUIRE(m.n < 92);
    m.fill_to(92);
    return m.bits(92);
}

/* MAC-DATA with SSI and length indication, filled to nbits. */
std::vector<uint8_t> mac_data(uint32_t ssi, const pdu &sdu, unsigned nbits)
{
    pdu m;
    unsigned len = 37 + sdu.n, li = (len + 7) / 8;

    m.put(0, 2);
    m.put(li * 8 != len, 1);
    m.put(0, 1);
    m.put(0, 2);
    m.put(ssi, 24);
    m.put(0, 1);
    m.put(li, 6);
    m.put_bits(sdu, 0, sdu.n);
    m.fill_to(li * 8);
    REQUIRE(m.n <= nbits);
    m.fill_to(nbits);
    return m.bits(nbits);
}

/* Random bits, deterministic. */
struct stream {
    std::vector<uint8_t> bits;
    uint32_t r = 0x9e3779b9u;

    void noise(unsigned n)
    {
        for (unsigned i = 0; i < n; i++) {
            r ^= r << 13;
            r ^= r >> 17;
            r ^= r << 5;
            bits.push_back(r & 1);
        }
    }
    /* One uplink slot: 34 bits of ramping, the burst, noise to 510. */
    void slot(const uint8_t *burst, unsigned n)
    {
        noise(34);
        bits.insert(bits.end(), burst, burst + n);
        noise(510 - 34 - n);
    }
    void cb(const std::vector<uint8_t> &t1)
    {
        uint8_t t5[168], burst[TETRA_CB_BITS];

        tetra_encode_sch(TETRA_SCH_HU, t1.data(), scramb(), t5);
        tetra_tmo_build_cb(burst, t5);
        slot(burst, TETRA_CB_BITS);
    }
    void schf(const std::vector<uint8_t> &t1)
    {
        uint8_t t5[432], burst[TETRA_NUB_BITS];

        tetra_encode_sch(TETRA_SCH_F, t1.data(), scramb(), t5);
        tetra_tmo_build_nub(burst, t5, t5 + 216, false);
        slot(burst, TETRA_NUB_BITS);
    }
    /* STCH in block 1, speech frame in block 2. */
    void stolen(const std::vector<uint8_t> &t1, const uint8_t *frame)
    {
        uint8_t b1[216], b2[216], burst[TETRA_NUB_BITS];

        tetra_encode_sch(TETRA_SCH_H, t1.data(), scramb(), b1);
        tetra_speech_encode_half(frame, b2);
        tetra_scramble(b2, 216, scramb());
        tetra_tmo_build_nub(burst, b1, b2, true);
        slot(burst, TETRA_NUB_BITS);
    }
    void speech(const uint8_t *a, const uint8_t *b, uint8_t type4[432])
    {
        uint8_t t5[432], burst[TETRA_NUB_BITS];

        tetra_speech_encode_slot(a, b, type4);
        std::memcpy(t5, type4, 432);
        tetra_scramble(t5, 432, scramb());
        tetra_tmo_build_nub(burst, t5, t5 + 216, false);
        slot(burst, TETRA_NUB_BITS);
    }
};

void frame(uint8_t f[137], unsigned seed)
{
    for (unsigned i = 0; i < 137; i++)
        f[i] = (uint8_t)(((i * 7 + seed * 13) >> 2) & 1);
}

} // namespace

TEST_CASE("Control uplink bursts carry random access PDUs", "[tmo][ul]")
{
    static ul_capture c;
    stream s;

    s.noise(5000);
    s.cb(mac_access(1234567, u_tx_demand(77)));
    s.noise(3000);
    s.cb(mac_access(7654321, u_setup(9001)));
    s.noise(3000);

    init(c);
    tetra_tmo_push_bits(&c.rx, s.bits.data(), s.bits.size());

    REQUIRE(c.calls.size() == 2);
    REQUIRE(c.calls[0].what == TETRA_CALL_TX_DEMAND);
    REQUIRE(c.calls[0].call_id == 77);
    REQUIRE(c.calls[0].src_ssi == 1234567);
    REQUIRE(c.calls[0].priority == 1);
    REQUIRE(c.calls[1].what == TETRA_CALL_SETUP);
    REQUIRE(c.calls[1].src_ssi == 7654321);
    REQUIRE(c.calls[1].dst_ssi == 9001);
    REQUIRE(c.calls[1].group);
    REQUIRE(c.calls[1].priority == 5);
    REQUIRE(c.uplink_events == c.events);
    REQUIRE(c.bad_time == 0);
    REQUIRE(!tetra_tmo_locked(&c.rx));
}

TEST_CASE("Uplink is not decoded without the right colour code", "[tmo][ul]")
{
    stream s;

    s.noise(5000);
    s.cb(mac_access(1234567, u_tx_demand(77)));
    s.schf(mac_data(2001, u_sds_text(3001, "HELLO"), 268));
    s.noise(3000);

    SECTION("unknown")
    {
        static ul_capture c;

        init(c, false);
        tetra_tmo_push_bits(&c.rx, s.bits.data(), s.bits.size());
        REQUIRE(c.uplink_events == 0);
    }
    SECTION("other cell")
    {
        static ul_capture c;

        init(c, false);
        tetra_tmo_set_uplink_cell(&c.rx, MCC, MNC, CC + 1);
        tetra_tmo_push_bits(&c.rx, s.bits.data(), s.bits.size());
        REQUIRE(c.uplink_events == 0);
    }
}

TEST_CASE("Normal uplink bursts carry SDS, also fragmented", "[tmo][ul]")
{
    static ul_capture c;
    const char *long_text = "An uplink SDS-TL text that is long enough to be "
                            "fragmented by the MS over several slots.";
    stream s;

    s.noise(4000);
    s.schf(mac_data(2001, u_sds_text(3001, "HELLO"), 268));

    /* MAC-DATA (capacity request, start of fragmentation), MAC-FRAG,
     * MAC-END with a length indication */
    pdu sdu = u_sds_text(3002, long_text);
    unsigned first = 268 - 37, off = 0;

    REQUIRE(sdu.n > first + 264);
    {
        pdu m;

        m.put(0, 2);
        m.put(0, 1);
        m.put(0, 1);
        m.put(0, 2);
        m.put(2002, 24);
        m.put(1, 1); /* capacity request */
        m.put(1, 1); /* start of fragmentation */
        m.put(3, 4);
        m.put(0, 1);
        m.put_bits(sdu, 0, first);
        off += first;
        s.schf(m.bits(268));
    }
    {
        pdu m;

        m.put(1, 2);
        m.put(0, 1);
        m.put(0, 1);
        m.put_bits(sdu, off, 264);
        off += 264;
        s.schf(m.bits(268));
    }
    {
        pdu m;
        unsigned rest = sdu.n - off, len = 10 + rest, li = (len + 7) / 8;

        REQUIRE(li <= 34);
        m.put(1, 2);
        m.put(1, 1);
        m.put(li * 8 != len, 1);
        m.put(li, 6);
        m.put_bits(sdu, off, rest);
        m.fill_to(li * 8);
        m.fill_to(268);
        s.schf(m.bits(268));
    }
    s.noise(3000);

    init(c);
    tetra_tmo_push_bits(&c.rx, s.bits.data(), s.bits.size());

    REQUIRE(c.sds.size() == 2);
    REQUIRE(c.sds[0].src_ssi == 2001);
    REQUIRE(c.sds[0].dst_ssi == 3001);
    REQUIRE(c.sds[1].src_ssi == 2002);
    REQUIRE(c.sds[1].dst_ssi == 3002);
    REQUIRE(c.texts.size() == 2);
    REQUIRE(c.texts[0] == "HELLO");
    REQUIRE(c.texts[1] == long_text);
    REQUIRE(c.bad_time == 0);
}

TEST_CASE("Uplink traffic follows the stolen signalling", "[tmo][ul]")
{
    static ul_capture c;
    uint8_t f[5][137], type4[3][432];
    stream s;

    for (unsigned i = 0; i < 5; i++)
        frame(f[i], i + 1);
    s.noise(4000);
    s.stolen(mac_data(1234567, u_tx_demand(42), 124), f[0]);
    for (unsigned i = 0; i < 2; i++)
        s.speech(f[1 + 2 * i], f[2 + 2 * i], type4[i]);
    s.noise(2000);

    init(c);
    tetra_tmo_push_bits(&c.rx, s.bits.data(), s.bits.size());

    REQUIRE(c.calls.size() == 1);
    REQUIRE(c.calls[0].what == TETRA_CALL_TX_DEMAND);
    REQUIRE(c.calls[0].call_id == 42);
    REQUIRE(c.traffic.size() >= 3);
    REQUIRE(c.traffic[0].half[0] == TETRA_HALF_STOLEN_C);
    REQUIRE(c.traffic[0].half[1] == TETRA_HALF_TCH);
    for (unsigned i = 0; i < 2; i++) {
        REQUIRE(c.traffic[1 + i].half[0] == TETRA_HALF_TCH);
        REQUIRE(c.tch[1 + i] == std::vector<uint8_t>(type4[i], type4[i] + 432));
    }

    /* The speech decoder gets the frames back */
    struct tetra_speech_ws ws;
    struct tetra_speech sp;

    /* bits is only valid during the callback: rebuild it */
    std::vector<int8_t> soft(432);
    for (unsigned i = 0; i < 432; i++)
        soft[i] = c.tch[1][i] ? TETRA_SOFT_ONE : TETRA_SOFT_ZERO;
    c.traffic[1].bits = soft.data();
    tetra_speech_decode(&ws, &c.traffic[1], &sp);
    REQUIRE(sp.bfi[0] == 0);
    REQUIRE(sp.bfi[1] == 0);
    for (unsigned i = 0; i < 137; i++) {
        REQUIRE(sp.bits[0][i] == f[1][i]);
        REQUIRE(sp.bits[1][i] == f[2][i]);
    }
}

TEST_CASE("Normal uplink bursts without signalling are not traffic",
          "[tmo][ul]")
{
    static ul_capture c;
    uint8_t f[2][137], type4[432];
    stream s;

    frame(f[0], 1);
    frame(f[1], 2);
    s.noise(4000);
    s.speech(f[0], f[1], type4);
    s.noise(2000);

    init(c);
    tetra_tmo_push_bits(&c.rx, s.bits.data(), s.bits.size());
    REQUIRE(c.traffic.empty());
}

TEST_CASE("Normal bursts that look like control bursts are decoded",
          "[tmo][ul]")
{
    /* Real normal bursts often have bits around n that complete an x */
    static const uint8_t xs[30] = {
        1, 0, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1,
        0, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 1, 1
    };
    static ul_capture c;
    stream s;
    uint8_t t5[432], burst[TETRA_NUB_BITS];
    std::vector<uint8_t> t1 = mac_data(2001, u_sds_text(3001, "XN"), 268);

    tetra_encode_sch(TETRA_SCH_F, t1.data(), scramb(), t5);
    tetra_tmo_build_nub(burst, t5, t5 + 216, false);
    std::memcpy(burst + 220 - 4, xs, 30); /* x around n */
    s.noise(4000);
    s.slot(burst, TETRA_NUB_BITS);
    s.noise(3000);

    init(c);
    tetra_tmo_push_bits(&c.rx, s.bits.data(), s.bits.size());
    REQUIRE(c.texts.size() == 1);
    REQUIRE(c.texts[0] == "XN");
}

TEST_CASE("n or p in noise is not taken for traffic", "[tmo][ul]")
{
    static ul_capture c;
    stream s;
    uint8_t blk[TETRA_NUB_BITS];

    s.noise(4000);
    s.schf(mac_data(2001, u_sds_text(3001, "HI"), 268)); /* opens window */
    /* Training sequences without a burst (no tail bits) */
    for (unsigned i = 0; i < 20; i++) {
        s.noise(TETRA_NUB_BITS);
        std::memcpy(blk, &s.bits[s.bits.size() - TETRA_NUB_BITS], sizeof(blk));
        s.bits.resize(s.bits.size() - TETRA_NUB_BITS);
        for (unsigned k = 0; k < 4; k++) { /* inverted tail bits */
            blk[k] = (uint8_t)(k >= 2);
            blk[458 + k] = (uint8_t)(k >= 2);
        }
        std::memcpy(blk + 220,
                    i & 1 ? "\0\1\1\1\1\0\1\0\0\1\0\0\0\0\1\1\0\1\1\1\1\0"
                          : "\1\1\0\1\0\0\0\0\1\1\1\0\1\0\0\1\1\1\0\1\0\0",
                    22);
        s.slot(blk, TETRA_NUB_BITS);
    }
    s.noise(2000);

    init(c);
    tetra_tmo_push_bits(&c.rx, s.bits.data(), s.bits.size());
    REQUIRE(c.texts.size() == 1);
    REQUIRE(c.traffic.empty());
}

#endif /* TETRA_UPLINK_DECODE */
