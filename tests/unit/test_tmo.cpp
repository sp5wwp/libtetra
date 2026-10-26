/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <tetra/tetra.h>
#include <tetra/tetra_coding.h>
#include <tetra/tetra_tx.h>

extern "C" {
#include "util/bits.h"
}

#include "../vectors/osmo_tmo_tch.h"

#ifndef TETRA_TEST_VECTOR_DIR
#define TETRA_TEST_VECTOR_DIR "tests/vectors"
#endif

namespace
{

struct capture {
    struct tetra_tmo_rx rx;
    unsigned locks = 0;
    struct tetra_cell cell;
    unsigned cells = 0;
    std::vector<struct tetra_call> calls;
    std::vector<struct tetra_sds> sds;
    std::vector<std::string> texts;
    std::vector<std::vector<uint8_t>> tch;
    std::vector<uint8_t> tch_um;
};

void on_event(void *user, const struct tetra_event *ev)
{
    capture *c = static_cast<capture *>(user);

    switch (ev->type) {
    case TETRA_EV_SYNC:
        if (ev->u.sync.locked)
            c->locks++;
        break;
    case TETRA_EV_CELL:
        c->cell = *ev->u.cell;
        c->cells++;
        break;
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
        c->tch.push_back(b);
        c->tch_um.push_back(ev->u.traffic.usage_marker);
        break;
    }
    default:
        break;
    }
}

void init(capture &c)
{
    struct tetra_rx_config cfg;

    std::memset(&cfg, 0, sizeof(cfg));
    cfg.cb = on_event;
    cfg.user = &c;
    cfg.promiscuous = true;
    tetra_tmo_init(&c.rx, &cfg);
}

/* ---- Synthetic TMO downlink ---- */

const uint16_t MCC = 901, MNC = 9999;
const uint8_t CC = 5;

struct tmo_gen {
    uint8_t tn = 1, fn = 17, mn = 1;
    bool synced = false; /* a sync burst has been sent */
    std::vector<uint8_t> bits;
    /* SCH/F blocks to send, in order, on TN1 of frames 1..17 */
    std::vector<std::vector<uint8_t>> schf;
};

uint32_t scramb()
{
    return tetra_scramb_init(tetra_tmo_colour_code(MCC, MNC, CC));
}

std::vector<uint8_t> unpack(const uint8_t *p, unsigned n)
{
    std::vector<uint8_t> b(n);

    for (unsigned i = 0; i < n; i++)
        b[i] = (uint8_t)bit_at(p, i);
    return b;
}

std::vector<uint8_t> null_block(unsigned nbits)
{
    uint8_t p[34] = { 0 };
    unsigned pos = 0;

    bw_put(p, &pos, 0, 2); /* MAC-RESOURCE */
    bw_put(p, &pos, 1, 1); /* fill bits */
    bw_put(p, &pos, 0, 4);
    bw_put(p, &pos, 2, 6); /* 2 octets */
    bw_put(p, &pos, 0, 3); /* null PDU */
    bw_put(p, &pos, 1, 1); /* fill */
    return unpack(p, nbits);
}

void gen_slot(tmo_gen &g)
{
    uint8_t slot[510], bb[30], b1[216], b2[216], t5[432];
    uint16_t aach = 0; /* header 00: common control */

    tetra_encode_aach(aach, scramb(), bb);

    if (g.fn == 18 && (g.mn + g.tn) % 4 == 3) {
        uint8_t p[16] = { 0 }, sb[120];
        unsigned pos = 0;

        bw_put(p, &pos, 1, 4);
        bw_put(p, &pos, CC, 6);
        bw_put(p, &pos, g.tn - 1u, 2);
        bw_put(p, &pos, g.fn, 5);
        bw_put(p, &pos, g.mn, 6);
        bw_put(p, &pos, 0, 2 + 3 + 1 + 1 + 1);
        bw_put(p, &pos, MCC, 10);
        bw_put(p, &pos, MNC, 14);
        bw_put(p, &pos, 0, 5);
        std::vector<uint8_t> t1 = unpack(p, 60);

        tetra_encode_sch(TETRA_SCH_S, t1.data(), tetra_scramb_init(0), sb);

        /* SYSINFO in block 2 */
        uint8_t q[16] = { 0 };

        pos = 0;
        bw_put(q, &pos, 2, 2);
        bw_put(q, &pos, 0, 2);
        bw_put(q, &pos, 3700, 12);
        bw_put(q, &pos, 3, 4);
        bw_put(q, &pos, 3, 2);
        bw_put(q, &pos, 0, 3 + 1 + 2 + 3 + 4 + 4 + 4);
        bw_put(q, &pos, 0, 1);
        bw_put(q, &pos, 66, 16);
        bw_put(q, &pos, 2, 2);
        bw_put(q, &pos, 0, 20);
        bw_put(q, &pos, 77, 14);
        bw_put(q, &pos, 0xffff, 16);
        bw_put(q, &pos, 0xd70, 12);
        std::vector<uint8_t> h = unpack(q, 124);

        tetra_encode_sch(TETRA_SCH_H, h.data(), scramb(), b2);
        tetra_tmo_build_sb(slot, sb, bb, b2);
        g.synced = true;
    } else if (g.synced && g.tn == 1 && g.fn != 18 && !g.schf.empty()) {
        tetra_encode_sch(TETRA_SCH_F, g.schf.front().data(), scramb(), t5);
        g.schf.erase(g.schf.begin());
        tetra_tmo_build_ndb(slot, t5, bb, t5 + 216, false);
    } else {
        std::vector<uint8_t> n = null_block(124);

        tetra_encode_sch(TETRA_SCH_H, n.data(), scramb(), b1);
        tetra_encode_sch(TETRA_SCH_H, n.data(), scramb(), b2);
        tetra_tmo_build_ndb(slot, b1, bb, b2, true);
    }
    g.bits.insert(g.bits.end(), slot, slot + 510);

    if (++g.tn > 4) {
        g.tn = 1;
        if (++g.fn > 18) {
            g.fn = 1;
            if (++g.mn > 60)
                g.mn = 1;
        }
    }
}

/* TM-SDU: BL-UDATA + MLE (CMCE) + CMCE PDU bits. */
struct pdu {
    uint8_t b[400] = { 0 };
    unsigned n = 0;
    void put(uint32_t v, unsigned k)
    {
        bw_put(b, &n, v, k);
    }
};

pdu cmce_sds_text(uint32_t src, const char *text)
{
    pdu p;
    unsigned len = (unsigned)std::strlen(text);
    unsigned septet_bits = 7 * len;
    unsigned octets = (septet_bits + 7) / 8;
    uint8_t packed[200] = { 0 };

    /* GSM 03.38 packing (all characters used here map to ASCII) */
    for (unsigned i = 0; i < len; i++) {
        unsigned v = (unsigned)text[i] & 0x7f, bit = 7 * i;

        packed[bit / 8] |= (uint8_t)(v << (bit % 8));
        if (bit % 8 > 1)
            packed[bit / 8 + 1] |= (uint8_t)(v >> (8 - bit % 8));
    }

    p.put(2, 4);  /* BL-UDATA */
    p.put(2, 3);  /* CMCE */
    p.put(15, 5); /* D-SDS-DATA */
    p.put(1, 2);  /* calling party SSI */
    p.put(src, 24);
    p.put(3, 2); /* UDD4 */
    p.put(8 + 4 + 2 + 1 + 1 + 8 + 1 + 7 + 8 * octets, 11);
    p.put(0x82, 8);
    p.put(0, 4 + 2 + 1 + 1);
    p.put(7, 8); /* message reference */
    p.put(0, 1);
    p.put(0, 7); /* GSM 7-bit */
    for (unsigned i = 0; i < octets; i++)
        p.put(packed[i], 8);
    p.put(0, 1); /* O-bit */
    return p;
}

pdu cmce_status(uint32_t src, uint16_t status)
{
    pdu p;

    p.put(2, 4);
    p.put(2, 3);
    p.put(8, 5); /* D-STATUS */
    p.put(1, 2);
    p.put(src, 24);
    p.put(status, 16);
    p.put(0, 1);
    return p;
}

/* MAC-RESOURCE header (SSI address, no optional elements): 43 bits. */
void mac_resource(pdu &blk, uint32_t ssi, unsigned li, bool fill)
{
    blk.put(0, 2);
    blk.put(fill, 1);
    blk.put(0, 1 + 2 + 1);
    blk.put(li, 6);
    blk.put(1, 3);
    blk.put(ssi, 24);
    blk.put(0, 3);
}

void put_bits(pdu &dst, const pdu &src, unsigned from, unsigned n)
{
    bits_copy(dst.b, dst.n, src.b, from, n);
    dst.n += n;
}

void fill_to(pdu &blk, unsigned end)
{
    if (blk.n >= end)
        return;
    blk.put(1, 1);
    while (blk.n < end)
        blk.put(0, 1);
}

/* Single MAC-RESOURCE with length indication, rounded up to octets. */
void add_resource(pdu &blk, uint32_t ssi, const pdu &sdu)
{
    unsigned start = blk.n, len = 43 + sdu.n;
    unsigned li = (len + 7) / 8;
    bool fill = li * 8 != len;

    mac_resource(blk, ssi, li, fill);
    put_bits(blk, sdu, 0, sdu.n);
    fill_to(blk, start + li * 8);
}

std::vector<uint8_t> finish(pdu &blk)
{
    fill_to(blk, 268);
    return unpack(blk.b, 268);
}

} // namespace

TEST_CASE("TMO receiver decodes a real downlink recording excerpt",
          "[tmo][oracle]")
{
    static capture c;
    std::vector<uint8_t> dibits;
    FILE *f = std::fopen(TETRA_TEST_VECTOR_DIR "/tmo_call.dibits", "rb");
    int ch;

    REQUIRE(f != nullptr);
    while ((ch = std::fgetc(f)) != EOF) {
        for (int k = 6; k >= 0; k -= 2)
            dibits.push_back((uint8_t)((ch >> k) & 3));
    }
    std::fclose(f);

    init(c);
    tetra_tmo_push_dibits(&c.rx, dibits.data(), dibits.size());

    REQUIRE(c.locks == 1);
    REQUIRE(c.cell.mcc == 235);
    REQUIRE(c.cell.mnc == 78);
    REQUIRE(c.cell.colour_code == 1);
    REQUIRE(c.cell.have_sysinfo);
    REQUIRE(c.cell.dl_freq_hz == 392512500u);

    REQUIRE(c.calls.size() >= 2);
    REQUIRE(c.calls[0].what == TETRA_CALL_SETUP);
    REQUIRE(c.calls[0].call_id == 8);
    REQUIRE(c.calls[0].dst_ssi == 9001);
    REQUIRE(c.calls[0].src_ssi == 320026);
    REQUIRE(c.calls[0].group);
    REQUIRE(c.calls[0].usage_marker == 8);
    REQUIRE(c.calls[0].timeslots == 0x2);
    REQUIRE(c.calls[1].what == TETRA_CALL_CONNECT);

    /* Traffic identical to osmo-tetra's decoding of the same signal */
    REQUIRE(c.tch.size() == 5);
    for (unsigned i = 0; i < 5; i++) {
        REQUIRE(c.tch_um[i] == 8);
        REQUIRE(c.tch[i] == unpack(osmo_tmo_call_tch[i], 432));
    }
}

TEST_CASE("TMO receiver handles SYSINFO, association, fragmentation and SDS",
          "[tmo]")
{
    static capture c;
    tmo_gen g;
    const char *long_text = "This is a TETRA SDS-TL text long enough to be "
                            "fragmented by the MAC over three slots.";

    /* Block 1: two associated PDUs, D-STATUS then a short text */
    {
        pdu blk;

        /* 104 + 144 bits: both fit in the 268-bit SCH/F block */
        add_resource(blk, 1111, cmce_status(4242, 32768 + 7));
        add_resource(blk, 1111, cmce_sds_text(4242, "HI"));
        g.schf.push_back(finish(blk));
    }
    /* Blocks 2..4: a fragmented text (MAC-RESOURCE, MAC-FRAG, MAC-END) */
    {
        pdu sdu = cmce_sds_text(5353, long_text), blk;
        unsigned first = 268 - 43, off = 0;

        REQUIRE(sdu.n > first + 264);
        mac_resource(blk, 2222, 0x3f, false);
        put_bits(blk, sdu, 0, first);
        off += first;
        g.schf.push_back(finish(blk));

        pdu frag;

        frag.put(1, 2);
        frag.put(0, 1);
        frag.put(0, 1);
        put_bits(frag, sdu, off, 264);
        off += 264;
        g.schf.push_back(finish(frag));

        pdu end;
        unsigned rest = sdu.n - off, len = 13 + rest;
        unsigned li = (len + 7) / 8;

        end.put(1, 2);
        end.put(1, 1);
        end.put(li * 8 != len, 1);
        end.put(0, 1);
        end.put(li, 6);
        end.put(0, 2);
        put_bits(end, sdu, off, rest);
        fill_to(end, li * 8);
        g.schf.push_back(finish(end));
    }

    for (unsigned i = 0; i < 4 * 18 * 2; i++)
        gen_slot(g);

    init(c);
    tetra_tmo_push_bits(&c.rx, g.bits.data(), g.bits.size());

    REQUIRE(c.locks == 1);
    REQUIRE(c.cell.mcc == MCC);
    REQUIRE(c.cell.mnc == MNC);
    REQUIRE(c.cell.colour_code == CC);
    REQUIRE(c.cell.location_area == 77);
    REQUIRE(c.cell.hyperframe == 66);
    REQUIRE(c.cell.dl_freq_hz == 392512500u);
    REQUIRE(c.cell.service_details == 0xd70);

    REQUIRE(c.sds.size() == 3);
    REQUIRE(c.sds[0].type == TETRA_SDS_STATUS);
    REQUIRE(c.sds[0].status == 32768 + 7);
    REQUIRE(c.sds[0].src_ssi == 4242);
    REQUIRE(c.sds[0].dst_ssi == 1111);
    REQUIRE(c.sds[1].type == TETRA_SDS_UDD4);
    REQUIRE(c.sds[2].src_ssi == 5353);
    REQUIRE(c.sds[2].dst_ssi == 2222);
    REQUIRE(c.texts.size() == 2);
    REQUIRE(c.texts[0] == "HI");
    REQUIRE(c.texts[1] == long_text);
}
