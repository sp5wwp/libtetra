/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <tetra/tetra.h>
#include <tetra/tetra_coding.h>
#include <tetra/tetra_tx.h>

extern "C" {
#include "util/bits.h"
#include "tmo_int.h"
}

#include "../vectors/osmo_tmo_tch.h"

#ifndef TETRA_TEST_VECTOR_DIR
#define TETRA_TEST_VECTOR_DIR "tests/vectors"
#endif

namespace
{

struct captured_pdu {
    enum tetra_proto proto;
    uint8_t type;
    uint32_t addr;
    bool encrypted;
    uint16_t nbits;
    std::vector<uint8_t> data;
};

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
    std::vector<captured_pdu> pdus;
    std::vector<uint8_t> traffic_keystream;
    bool traffic_encrypted = false;
    unsigned uplinks = 0;
    std::vector<struct tetra_retune> retunes;
    bool accept_retune = true;
    unsigned uplink_flagged = 0;
};

bool on_retune(void *user, const struct tetra_retune *r)
{
    capture *c = static_cast<capture *>(user);

    c->retunes.push_back(*r);
    return c->accept_retune;
}

void on_event(void *user, const struct tetra_event *ev)
{
    capture *c = static_cast<capture *>(user);

    if (ev->uplink)
        c->uplink_flagged++;
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
        c->traffic_encrypted = ev->u.traffic.encrypted;
        c->traffic_keystream.clear();
        if (ev->u.traffic.cipher_keystream) {
            c->traffic_keystream.assign(
                ev->u.traffic.cipher_keystream,
                ev->u.traffic.cipher_keystream + TETRA_TCH_KSS_BITS);
        }
        break;
    }
    case TETRA_EV_PDU: {
        captured_pdu pdu;

        pdu.proto = ev->u.pdu.proto;
        pdu.type = ev->u.pdu.type;
        pdu.addr = ev->u.pdu.addr;
        pdu.encrypted = ev->u.pdu.encrypted;
        pdu.nbits = ev->u.pdu.nbits;
        if (ev->u.pdu.data && ev->u.pdu.nbits) {
            unsigned nbytes = (ev->u.pdu.nbits + 7u) / 8u;
            pdu.data.assign(ev->u.pdu.data, ev->u.pdu.data + nbytes);
        }
        c->pdus.push_back(pdu);
        break;
    }
    case TETRA_EV_SIGNAL:
        if (ev->u.signal.type == TETRA_SIG_TMO_UPLINK)
            c->uplinks++;
        break;
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

void make_sysinfo_block(uint8_t blk[34], uint16_t carrier, uint16_t la,
                        bool is_cck, uint16_t hf)
{
    unsigned pos = 0;

    std::memset(blk, 0, 34);
    bw_put(blk, &pos, 2, 2); /* MAC PDU type: SYSINFO */
    bw_put(blk, &pos, 0, 2); /* SYSINFO subtype */
    bw_put(blk, &pos, carrier, 12);
    bw_put(blk, &pos, 3, 4); /* band */
    bw_put(blk, &pos, 3, 2); /* offset */
    bw_put(blk, &pos, 0, 3 + 1 + 2 + 3 + 4 + 4 + 4);
    bw_put(blk, &pos, is_cck, 1);
    bw_put(blk, &pos, hf, 16);
    bw_put(blk, &pos, 0, 2 + 20);
    bw_put(blk, &pos, la, 14);
    bw_put(blk, &pos, 0xffff, 16);
    bw_put(blk, &pos, 0xd70, 12);
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
    REQUIRE(c.uplinks == 0);
    REQUIRE(c.cell.mcc == 235);
    REQUIRE(c.cell.mnc == 78);
    REQUIRE(c.cell.colour_code == 1);
    REQUIRE(c.cell.have_sysinfo);
    REQUIRE(c.cell.dl_freq_hz == 392512500u);

    /* D-CALL PROCEEDING to the caller comes before the D-SETUP */
    REQUIRE(c.calls.size() >= 3);
    REQUIRE(c.calls[0].what == TETRA_CALL_PROCEEDING);
    REQUIRE(c.calls[0].call_id == 8);
    REQUIRE(c.calls[0].src_ssi == 320026);
    REQUIRE(c.calls[0].status == TETRA_CALL_ST_PROCEEDING);
    REQUIRE(c.calls[1].what == TETRA_CALL_SETUP);
    REQUIRE(c.calls[1].call_id == 8);
    REQUIRE(c.calls[1].dst_ssi == 9001);
    REQUIRE(c.calls[1].src_ssi == 320026);
    REQUIRE(c.calls[1].group);
    REQUIRE(c.calls[1].usage_marker == 8);
    REQUIRE(c.calls[1].timeslots == 0x2);
    REQUIRE(c.calls[1].status == TETRA_CALL_ST_NONE);
    REQUIRE(c.calls[2].what == TETRA_CALL_CONNECT);

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

/* ---- Carrier following ---- */

namespace
{

/* D-SETUP to a group with a channel allocation on another carrier. */
void add_setup_ca(pdu &blk, uint32_t gssi, uint16_t id, uint16_t carrier,
                  unsigned ts)
{
    pdu sdu;
    unsigned start = blk.n, len, li;

    sdu.put(2, 4);
    sdu.put(2, 3);
    sdu.put(7, 5); /* D-SETUP */
    sdu.put(id, 14);
    sdu.put(0, 4 + 1 + 1);
    sdu.put(1 << 2, 8); /* speech, group */
    sdu.put(0, 2 + 1);  /* granted */
    sdu.put(0, 4);
    sdu.put(0, 1); /* O-bit */

    len = 43 + 25 + sdu.n;
    li = (len + 7) / 8;
    blk.put(0, 2);
    blk.put(li * 8 != len, 1);
    blk.put(0, 1 + 2 + 1);
    blk.put(li, 6);
    blk.put(1, 3);
    blk.put(gssi, 24);
    blk.put(0, 2);
    blk.put(1, 1); /* channel allocation */
    blk.put(0, 2); /* replace */
    blk.put(ts, 4);
    blk.put(3, 2); /* uplink and downlink */
    blk.put(0, 2);
    blk.put(carrier, 12);
    blk.put(0, 1); /* no extended carrier numbering */
    blk.put(1, 2); /* monitoring pattern */
    put_bits(blk, sdu, 0, sdu.n);
    fill_to(blk, start + li * 8);
}

/* End the block with a null PDU, as a base station does. */
std::vector<uint8_t> finish_null(pdu &blk)
{
    if (blk.n + 16 <= 268) {
        blk.put(0, 2); /* MAC-RESOURCE */
        blk.put(0, 1 + 4);
        blk.put(2, 6); /* 2 octets */
        blk.put(0, 3); /* null address */
    }
    return finish(blk);
}

pdu cmce_release(uint16_t id)
{
    pdu p;

    p.put(2, 4);
    p.put(2, 3);
    p.put(6, 5); /* D-RELEASE */
    p.put(id, 14);
    p.put(1, 5);
    p.put(0, 1);
    return p;
}

void init_retune(capture &c, bool accept)
{
    struct tetra_rx_config cfg;

    std::memset(&cfg, 0, sizeof(cfg));
    cfg.cb = on_event;
    cfg.user = &c;
    cfg.promiscuous = true;
    cfg.retune = on_retune;
    c.retunes.clear();
    c.accept_retune = accept;
    tetra_tmo_init(&c.rx, &cfg);
}

std::vector<uint8_t> call_on_carrier(bool release)
{
    tmo_gen g;
    pdu a, b;

    add_setup_ca(a, 5000, 321, 3705, 0x4); /* TN2 (MSB is TN1) */
    g.schf.push_back(finish_null(a));
    if (release) {
        add_resource(b, 5000, cmce_release(321));
        g.schf.push_back(finish_null(b));
    }
    for (unsigned i = 0; i < 4 * 18 * 2; i++)
        gen_slot(g);
    return g.bits;
}

} // namespace

TEST_CASE("TMO receiver follows a call to its carrier and back",
          "[tmo][retune]")
{
    static capture c;
    std::vector<uint8_t> bits = call_on_carrier(true);

    init_retune(c, true);
    tetra_tmo_push_bits(&c.rx, bits.data(), bits.size());

    REQUIRE(c.locks == 1);
    REQUIRE(c.cell.ul_freq_hz == 382512500u);
    REQUIRE(c.retunes.size() == 2);
    REQUIRE(c.retunes[0].reason == TETRA_RETUNE_CALL);
    REQUIRE(c.retunes[0].carrier == 3705);
    REQUIRE(c.retunes[0].freq_hz == 392637500u);
    REQUIRE(c.retunes[0].ul_freq_hz == 382637500u);
    REQUIRE(c.retunes[0].call_id == 321);
    REQUIRE(c.retunes[0].dst_ssi == 5000);
    REQUIRE(c.retunes[0].timeslots == TETRA_TS2);
    REQUIRE(c.retunes[1].reason == TETRA_RETUNE_MAIN);
    REQUIRE(c.retunes[1].carrier == 3700);
    REQUIRE(c.retunes[1].freq_hz == 392512500u);
    REQUIRE(c.uplink_flagged == 0);
}

TEST_CASE("TMO receiver stays when a retune is declined", "[tmo][retune]")
{
    static capture c;
    std::vector<uint8_t> bits = call_on_carrier(true);

    init_retune(c, false);
    tetra_tmo_push_bits(&c.rx, bits.data(), bits.size());

    REQUIRE(c.retunes.size() == 1);
    REQUIRE(c.retunes[0].reason == TETRA_RETUNE_CALL);
}

TEST_CASE("TMO receiver returns to the main carrier after losing lock",
          "[tmo][retune]")
{
    static capture c;
    std::vector<uint8_t> bits = call_on_carrier(false);

    /* The carrier disappears */
    bits.insert(bits.end(), 200 * 510, 0);
    init_retune(c, true);
    tetra_tmo_push_bits(&c.rx, bits.data(), bits.size());

    REQUIRE(!tetra_tmo_locked(&c.rx));
    REQUIRE(c.retunes.size() == 2);
    REQUIRE(c.retunes[1].reason == TETRA_RETUNE_MAIN);
    REQUIRE(c.retunes[1].freq_hz == 392512500u);
}

TEST_CASE("TMO resync drops the lock without a retune", "[tmo][retune]")
{
    static capture c;
    std::vector<uint8_t> bits = call_on_carrier(false);

    init_retune(c, true);
    tetra_tmo_push_bits(&c.rx, bits.data(), bits.size());
    REQUIRE(tetra_tmo_locked(&c.rx));
    REQUIRE(c.retunes.size() == 1);
    tetra_tmo_resync(&c.rx);
    REQUIRE(!tetra_tmo_locked(&c.rx));
    REQUIRE(c.retunes.size() == 1);
}

TEST_CASE("TMO receiver decodes selected timeslots only", "[tmo][oracle]")
{
    std::vector<uint8_t> dibits;
    FILE *f = std::fopen(TETRA_TEST_VECTOR_DIR "/tmo_call.dibits", "rb");
    int ch;

    REQUIRE(f != nullptr);
    while ((ch = std::fgetc(f)) != EOF) {
        for (int k = 6; k >= 0; k -= 2)
            dibits.push_back((uint8_t)((ch >> k) & 3));
    }
    std::fclose(f);

    SECTION("call slot only")
    {
        static capture c;

        init(c);
        tetra_tmo_set_slots(&c.rx, TETRA_TS2);
        tetra_tmo_push_dibits(&c.rx, dibits.data(), dibits.size());
        REQUIRE(c.locks == 1);
        REQUIRE(c.cell.mcc == 235);
        REQUIRE(c.calls.empty());
        REQUIRE(c.tch.size() == 5);
        for (unsigned i = 0; i < 5; i++)
            REQUIRE(c.tch[i] == unpack(osmo_tmo_call_tch[i], 432));
    }

    SECTION("control channel only")
    {
        static capture c;

        init(c);
        tetra_tmo_set_slots(&c.rx, TETRA_TS1);
        tetra_tmo_push_dibits(&c.rx, dibits.data(), dibits.size());
        REQUIRE(c.locks == 1);
        REQUIRE(c.calls.size() >= 2);
        REQUIRE(c.calls[1].what == TETRA_CALL_SETUP);
        REQUIRE(c.calls[1].timeslots == TETRA_TS2);
        REQUIRE(c.tch.empty());
    }
}

/* ---- Uplink recognition ---- */

#if TETRA_UPLINK_DETECT

namespace
{

const uint8_t seq_n[22] = { 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1,
                            0, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0 };
const uint8_t seq_x[30] = { 1, 0, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 1,
                            0, 1, 0, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0, 1, 1 };

enum ul_burst { UL_NONE, UL_CB, UL_NUB };

/*
 * Random bits with an uplink burst every TDMA frame from second 3 on:
 * control uplink bursts (x at burst bit 88) or normal uplink bursts
 * (n at burst bit 220), starting 34 bits into slot 1.
 */
std::vector<uint8_t> uplink_bits(enum ul_burst kind, unsigned seconds)
{
    std::vector<uint8_t> b(36000 * seconds);
    uint32_t r = 12345;

    for (auto &v : b) {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        v = r & 1;
    }
    for (size_t f = 3 * 36000; f + 2040 < b.size(); f += 2040) {
        if (kind == UL_CB)
            std::memcpy(&b[f + 34 + 88], seq_x, 30);
        else if (kind == UL_NUB)
            std::memcpy(&b[f + 34 + 220], seq_n, 22);
    }
    return b;
}

} // namespace

TEST_CASE("TMO receiver recognises uplink transmissions", "[tmo]")
{
    for (enum ul_burst kind : { UL_CB, UL_NUB }) {
        static capture c;
        std::vector<uint8_t> b = uplink_bits(kind, 15);

        c.uplinks = 0;
        init(c);
        tetra_tmo_push_bits(&c.rx, b.data(), b.size());
        REQUIRE(c.locks == 0);
        REQUIRE(c.uplinks == 1);
    }
}

TEST_CASE("Random bits are not taken for an uplink", "[tmo]")
{
    static capture c;
    std::vector<uint8_t> b = uplink_bits(UL_NONE, 120);

    init(c);
    tetra_tmo_push_bits(&c.rx, b.data(), b.size());
    REQUIRE(c.uplinks == 0);
}
#endif


TEST_CASE("TMO SYNC cell change clears serving-cell associations",
          "[tmo][cell]")
{
    struct tetra_tmo_rx rx{};
    uint8_t blk[8] = {};
    unsigned pos = 0;

    rx.cell.mcc = 902;
    rx.cell.mnc = 1234;
    rx.cell.colour_code = 1;
    rx.cell.system_code = 1;
    rx.cell.sharing_mode = 0;
    rx.frag[0].active = true;
#if TETRA_ENABLE_CRYPTO
    rx.frag[0].eck[0] = 0xa5;
#endif
    rx.calls[0].used = true;
    rx.have_network = true;
    rx.dl_um[0] = 7;

    bw_put(blk, &pos, 1, 4);       /* system code */
    bw_put(blk, &pos, CC, 6);
    bw_put(blk, &pos, 0, 2);       /* timeslot */
    bw_put(blk, &pos, 1, 5);       /* frame */
    bw_put(blk, &pos, 1, 6);       /* multiframe */
    bw_put(blk, &pos, 0, 2);       /* sharing mode */
    bw_put(blk, &pos, 0, 3 + 1 + 1 + 1);
    bw_put(blk, &pos, MCC, 10);
    bw_put(blk, &pos, MNC, 14);

    tmo_mac_sync(&rx, blk);
    REQUIRE(rx.cell.mcc == MCC);
    REQUIRE(rx.cell.mnc == MNC);
    REQUIRE(rx.cell.colour_code == CC);
    REQUIRE_FALSE(rx.frag[0].active);
#if TETRA_ENABLE_CRYPTO
    REQUIRE(rx.frag[0].eck[0] == 0);
#endif
    REQUIRE_FALSE(rx.calls[0].used);
    REQUIRE_FALSE(rx.have_network);
    REQUIRE(rx.dl_um[0] == 0xff);
}

TEST_CASE("TMO SYSINFO changes invalidate old call and fragment context",
          "[tmo][cell]")
{
    struct tetra_tmo_rx rx{};
    uint8_t blk[34];

    rx.cell.have_sysinfo = true;
    rx.cell.main_carrier = 3700;
    rx.cell.band = 3;
    rx.cell.offset = 3;
    rx.cell.location_area = 77;
    rx.cell.hyperframe = 66;
    rx.cell.have_hyperframe = true;
    rx.cell.cck_id = 9;
    rx.cell.have_cck_id = true;
    rx.frag[0].active = true;
#if TETRA_ENABLE_CRYPTO
    rx.frag[0].eck[0] = 0xa5;
#endif
    rx.calls[0].used = true;
    rx.have_network = true;

    /* Main-carrier change affects TB5; it invalidates ECKs and call state. */
    make_sysinfo_block(blk, 3701, 77, false, 66);
    tmo_mac_block(&rx, TMO_SCH_F, blk, 124, 0);
    REQUIRE(rx.cell.main_carrier == 3701);
    REQUIRE_FALSE(rx.frag[0].active);
#if TETRA_ENABLE_CRYPTO
    REQUIRE(rx.frag[0].eck[0] == 0);
#endif
    REQUIRE_FALSE(rx.calls[0].used);
    REQUIRE_FALSE(rx.have_network);

    /* A new CCK ID can select a different CK even with unchanged TB5 inputs. */
    rx.frag[0].active = true;
#if TETRA_ENABLE_CRYPTO
    rx.frag[0].eck[0] = 0x5a;
#endif
    rx.calls[0].used = true;
    make_sysinfo_block(blk, 3701, 77, true, 10);
    tmo_mac_block(&rx, TMO_SCH_F, blk, 124, 0);
    REQUIRE(rx.cell.cck_id == 10);
    REQUIRE_FALSE(rx.frag[0].active);
#if TETRA_ENABLE_CRYPTO
    REQUIRE(rx.frag[0].eck[0] == 0);
#endif
    REQUIRE_FALSE(rx.calls[0].used);

    /* LA is the other TB5 input and must also invalidate cached state. */
    rx.frag[0].active = true;
#if TETRA_ENABLE_CRYPTO
    rx.frag[0].eck[0] = 0x3c;
#endif
    rx.calls[0].used = true;
    make_sysinfo_block(blk, 3701, 78, false, 66);
    tmo_mac_block(&rx, TMO_SCH_F, blk, 124, 0);
    REQUIRE(rx.cell.location_area == 78);
    REQUIRE_FALSE(rx.frag[0].active);
#if TETRA_ENABLE_CRYPTO
    REQUIRE(rx.frag[0].eck[0] == 0);
#endif
    REQUIRE_FALSE(rx.calls[0].used);
}

TEST_CASE("TMO SYSINFO notification compares cell fields, not padding",
          "[tmo][cell]")
{
    capture c;
    uint8_t blk[34];

    init(c);

    make_sysinfo_block(blk, 3701, 77, false, 66);
    tmo_mac_block(&c.rx, TMO_SCH_F, blk, 124, 0);
    REQUIRE(c.cells == 1);

    /* Hyperframe-only updates are stored but do not emit a cell event. */
    make_sysinfo_block(blk, 3701, 77, false, 67);
    tmo_mac_block(&c.rx, TMO_SCH_F, blk, 124, 0);
    REQUIRE(c.cells == 1);
    REQUIRE(c.rx.cell.hyperframe == 67);

    /* A real serving-cell field change still emits an updated event. */
    make_sysinfo_block(blk, 3701, 78, false, 68);
    tmo_mac_block(&c.rx, TMO_SCH_F, blk, 124, 0);
    REQUIRE(c.cells == 2);
    REQUIRE(c.cell.location_area == 78);
}

TEST_CASE("TMO local TDMA clock advances the hyperframe", "[tmo][crypto]")
{
    struct tetra_tmo_rx rx{};

    rx.t.tn = 4;
    rx.t.fn = 18;
    rx.t.mn = 60;
    rx.cell.have_hyperframe = true;
    rx.cell.hyperframe = 0x1234;
    tmo_advance_time(&rx);

    REQUIRE(rx.t.tn == 1);
    REQUIRE(rx.t.fn == 1);
    REQUIRE(rx.t.mn == 1);
    REQUIRE(rx.cell.hyperframe == 0x1235);

    rx.t.tn = 4;
    rx.t.fn = 18;
    rx.t.mn = 60;
    rx.cell.hyperframe = 0xffff;
    tmo_advance_time(&rx);
    REQUIRE(rx.cell.hyperframe == 0);

    rx.t.tn = 4;
    rx.t.fn = 18;
    rx.t.mn = 60;
    rx.cell.have_hyperframe = false;
    rx.cell.hyperframe = 0x2222;
    tmo_advance_time(&rx);
    REQUIRE(rx.cell.hyperframe == 0x2222);
}

TEST_CASE("TMO encryption mode preserves the defined two-bit states",
          "[tmo][crypto]")
{
    struct tmo_macinfo mi{};

    mi.encryption_mode = TETRA_TMO_ENC_CLEAR;
    REQUIRE_FALSE(tmo_macinfo_encrypted(&mi));
    mi.encryption_mode = TETRA_TMO_ENC_RESERVED;
    REQUIRE_FALSE(tmo_macinfo_encrypted(&mi));
    mi.encryption_mode = TETRA_TMO_ENC_ENCRYPTED_EVEN;
    REQUIRE(tmo_macinfo_encrypted(&mi));
    mi.encryption_mode = TETRA_TMO_ENC_ENCRYPTED_ODD;
    REQUIRE(tmo_macinfo_encrypted(&mi));
}

#if TETRA_ENABLE_CRYPTO
namespace
{
struct key_request_capture {
    uint8_t encryption_mode = 0;
    bool encryption_mode_valid = false;
    bool traffic = false;
    bool provide_key = false;
    unsigned calls = 0;
    std::array<uint8_t, TETRA_TEA_KEY_BYTES> ck = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    };
};

bool capture_key_request(void *user, const tetra_tmo_key_request *request,
                         tetra_tea_algorithm *algorithm,
                         uint8_t ck[TETRA_TEA_KEY_BYTES])
{
    auto *capture = static_cast<key_request_capture *>(user);
    capture->encryption_mode = request->encryption_mode;
    capture->encryption_mode_valid = request->encryption_mode_valid;
    capture->traffic = request->traffic;
    capture->calls++;
    if (!capture->provide_key)
        return false;
    *algorithm = TETRA_TEA1;
    std::memcpy(ck, capture->ck.data(), capture->ck.size());
    return true;
}
} // namespace

TEST_CASE("TMO key provider receives the raw encryption mode", "[tmo][crypto]")
{
    struct tetra_tmo_rx rx{};
    key_request_capture capture;
    struct tetra_tmo_key_request request{};
    enum tetra_tea_algorithm algorithm = TETRA_TEA1;
    uint8_t eck[TETRA_TEA_KEY_BYTES] = {};

    rx.cell.have_sysinfo = true;
    rx.cell.have_hyperframe = true;
    tetra_tmo_set_key_provider(&rx, capture_key_request, &capture);
    request.cell = &rx.cell;
    request.encryption_mode = TETRA_TMO_ENC_ENCRYPTED_ODD;
    request.encryption_mode_valid = true;
    request.traffic = true;

    REQUIRE_FALSE(tmo_crypto_get_eck(&rx, &request, &algorithm, eck));
    REQUIRE(capture.encryption_mode == TETRA_TMO_ENC_ENCRYPTED_ODD);
    REQUIRE(capture.encryption_mode_valid);
    REQUIRE(capture.traffic);
}

TEST_CASE("TMO receiver decrypts MAC signalling and supplies traffic KSS",
          "[tmo][crypto][integration]")
{
    capture c;
    key_request_capture keys;
    uint8_t blk[34] = {};
    uint8_t eck[TETRA_TEA_KEY_BYTES] = {};
    uint8_t stream[2] = {};
    uint32_t iv = 0;
    unsigned pos = 0;
    const uint32_t address = 0x123456;
    const uint16_t carrier = 0xabc;
    const uint16_t location_area = 0x1234;
    const uint8_t colour_code = 0x2d;

    init(c);
    c.rx.t = { 1, 1, 1 };
    c.rx.cell.have_sysinfo = true;
    c.rx.cell.main_carrier = carrier;
    c.rx.cell.location_area = location_area;
    c.rx.cell.colour_code = colour_code;
    c.rx.cell.have_hyperframe = true;
    c.rx.cell.hyperframe = 0x1234;
    keys.provide_key = true;
    tetra_tmo_set_key_provider(&c.rx, capture_key_request, &keys);

    REQUIRE(tetra_tb5(carrier, location_area, colour_code, keys.ck.data(),
                      eck));
    REQUIRE(tetra_tea_build_iv(&c.rx.t, c.rx.cell.hyperframe,
                               TETRA_TEA_DOWNLINK, &iv));
    REQUIRE(tetra_tea_generate(TETRA_TEA1, iv, eck, sizeof(stream), stream));

    /* A 56-bit MAC-RESOURCE with a 13-bit LLC PDU payload. */
    bw_put(blk, &pos, 0, 2);       /* MAC-RESOURCE */
    bw_put(blk, &pos, 0, 1);       /* no fill bits */
    bw_put(blk, &pos, 0, 1);       /* no grant position */
    bw_put(blk, &pos, 2, 2);       /* encrypted, even key identifier */
    bw_put(blk, &pos, 0, 1);       /* no random access */
    bw_put(blk, &pos, 7, 6);       /* seven octets */
    bw_put(blk, &pos, 1, 3);       /* SSI address */
    bw_put(blk, &pos, address, 24);
    bw_put(blk, &pos, 0, 1);       /* no power control */
    bw_put(blk, &pos, 0, 1);       /* no basic slot grant */
    bw_put(blk, &pos, 0, 1);       /* no channel allocation */
    const unsigned payload_start = pos;
    bw_put(blk, &pos, 8, 4);       /* unhandled LLC PDU type */
    bw_put(blk, &pos, 0x12d, 9);   /* known payload bits */
    REQUIRE(pos == 56);

    /* Encrypt the SDU bits as received over the air. */
    for (unsigned i = 0; i < 13; i++) {
        unsigned k = (stream[i >> 3] >> (7u - (i & 7u))) & 1u;
        if (k)
            blk[(payload_start + i) >> 3] ^=
                (uint8_t)(0x80u >> ((payload_start + i) & 7u));
    }

    tmo_mac_block(&c.rx, TMO_SCH_F, blk, 56, 0);
    REQUIRE(keys.calls == 1);
    REQUIRE_FALSE(keys.traffic);
    REQUIRE(c.pdus.size() == 1);
    REQUIRE(c.pdus[0].proto == TETRA_PROTO_LLC);
    REQUIRE(c.pdus[0].type == 8);
    REQUIRE(c.pdus[0].addr == address);
    REQUIRE_FALSE(c.pdus[0].encrypted);
    REQUIRE(c.pdus[0].nbits == 13);
    REQUIRE(c.pdus[0].data.size() == 2);
    REQUIRE(bit_at(c.pdus[0].data.data(), 0) == 1);
    for (unsigned i = 0; i < 13; i++) {
        unsigned expected = i < 4 ? ((8u >> (3u - i)) & 1u)
                                  : ((0x12du >> (8u - (i - 4u))) & 1u);
        REQUIRE(bit_at(c.pdus[0].data.data(), i) == expected);
    }

    /* Traffic uses the same TB5/TEA path and exposes the full 353-bit KSS. */
    c.rx.calls[0].used = true;
    c.rx.calls[0].encrypted = true;
    c.rx.calls[0].encryption_mode = TETRA_TMO_ENC_ENCRYPTED_ODD;
    c.rx.calls[0].encryption_mode_valid = true;
    c.rx.calls[0].usage_marker = 8;
    c.rx.calls[0].dst_ssi = address;
    tmo_traffic(&c.rx, 8, TETRA_HALF_TCH, TETRA_HALF_TCH);
    REQUIRE(keys.calls == 2);
    REQUIRE(keys.traffic);
    REQUIRE(keys.encryption_mode == TETRA_TMO_ENC_ENCRYPTED_ODD);
    REQUIRE(keys.encryption_mode_valid);
    REQUIRE(c.traffic_encrypted);
    REQUIRE(c.traffic_keystream.size() == TETRA_TCH_KSS_BITS);

    uint8_t expected_stream[(TETRA_TCH_KSS_BITS + 7u) / 8u] = {};
    REQUIRE(tetra_tea_generate(TETRA_TEA1, iv, eck, sizeof(expected_stream),
                               expected_stream));
    for (unsigned i = 0; i < TETRA_TCH_KSS_BITS; i++) {
        unsigned expected =
            (expected_stream[i >> 3] >> (7u - (i & 7u))) & 1u;
        REQUIRE(c.traffic_keystream[i] == expected);
    }
}

namespace
{
/* Oracle CCK: encrypts SSI 0x0a1b2c to ESI 0xc40a49 (osmo-tetra ta61()). */
bool provide_test_cck(void *user, const struct tetra_cell *cell,
                      uint8_t cck[TETRA_TEA_KEY_BYTES])
{
    static const uint8_t key[TETRA_TEA_KEY_BYTES] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
    };
    (void)cell;
    if (user)
        *static_cast<unsigned *>(user) += 1;
    std::memcpy(cck, key, TETRA_TEA_KEY_BYTES);
    return true;
}

/* Encrypted MAC-RESOURCE (SSI address = esi) with an unhandled LLC PDU
 * payload, so the receiver reports it with the identity it resolved. */
void build_esi_block(uint8_t blk[34], uint32_t esi)
{
    unsigned pos = 0;

    std::memset(blk, 0, 34);
    bw_put(blk, &pos, 0, 2); /* MAC-RESOURCE */
    bw_put(blk, &pos, 0, 1); /* no fill bits */
    bw_put(blk, &pos, 0, 1); /* no grant position */
    bw_put(blk, &pos, 2, 2); /* encrypted, even key identifier */
    bw_put(blk, &pos, 0, 1); /* no random access */
    bw_put(blk, &pos, 7, 6); /* seven octets */
    bw_put(blk, &pos, 1, 3); /* SSI address */
    bw_put(blk, &pos, esi, 24);
    bw_put(blk, &pos, 0, 1); /* no power control */
    bw_put(blk, &pos, 0, 1); /* no basic slot grant */
    bw_put(blk, &pos, 0, 1); /* no channel allocation */
    bw_put(blk, &pos, 8, 4); /* unhandled LLC PDU type (left encrypted) */
    bw_put(blk, &pos, 0x12d, 9);
}
} // namespace

TEST_CASE("TMO receiver decrypts the MAC identity with TA61",
          "[tmo][crypto][ta61]")
{
    const uint32_t esi = 0xc40a49, ssi = 0x0a1b2c;
    uint8_t blk[34];

    build_esi_block(blk, esi);

    SECTION("CCK provider resolves the cleartext identity")
    {
        capture c;
        unsigned cck_calls = 0;

        init(c);
        tetra_tmo_set_cck_provider(&c.rx, provide_test_cck, &cck_calls);
        tmo_mac_block(&c.rx, TMO_SCH_F, blk, 56, 0);

        REQUIRE(cck_calls == 1);
        REQUIRE(c.pdus.size() == 1);
        REQUIRE(c.pdus[0].proto == TETRA_PROTO_LLC);
        REQUIRE(c.pdus[0].encrypted); /* no key provider: payload opaque */
        REQUIRE(c.pdus[0].addr == ssi);
    }

    SECTION("without a CCK provider the on-air identity is reported")
    {
        capture c;

        init(c);
        tmo_mac_block(&c.rx, TMO_SCH_F, blk, 56, 0);
        REQUIRE(c.pdus.size() == 1);
        REQUIRE(c.pdus[0].addr == esi);
    }
}
#endif

TEST_CASE("TMO receiver re-aligns its slot grid after a jump in the stream",
          "[tmo]")
{
    static capture c;

    /* Bits lost after three multiframes: odd and even counts, less and
     * more than a slot. Two would stay within the timing search. */
    for (unsigned drop : { 1u, 6u, 37u, 255u, 509u, 1000u, 3152u }) {
        tmo_gen g;

        for (unsigned i = 0; i < 4 * 18 * 12; i++)
            gen_slot(g);
        const size_t cut = 510u * 4 * 18 * 3 + 123;
        std::vector<uint8_t> bits(g.bits.begin(), g.bits.begin() + cut);
        bits.insert(bits.end(), g.bits.begin() + cut + drop, g.bits.end());

        INFO("bits dropped: " << drop);
        c.locks = 0;
        init(c);
        tetra_tmo_push_bits(&c.rx, bits.data(), cut);
        REQUIRE(c.locks == 1);
        const uint32_t before = c.rx.crc_ok;

        /* The next sync burst comes within a multiframe and moves the grid
         * onto the bursts again: the lock is kept */
        const size_t mid = cut + 510u * 4 * 18 * 2;
        tetra_tmo_push_bits(&c.rx, bits.data() + cut, mid - cut);
        REQUIRE(c.rx.realigned == 1);
        REQUIRE(c.rx.realign_shift <= -(int32_t)drop + 4);
        REQUIRE(c.rx.realign_shift >= -(int32_t)drop - 4);
        REQUIRE(c.locks == 1);
        REQUIRE(tetra_tmo_locked(&c.rx));

        /* and the blocks decode as before the jump: four slots a frame */
        const uint32_t mid_ok = c.rx.crc_ok;
        tetra_tmo_push_bits(&c.rx, bits.data() + mid, bits.size() - mid);
        REQUIRE(mid_ok > before);
        REQUIRE(c.rx.crc_ok - mid_ok > 4 * 18 * 6);
        REQUIRE(c.rx.realigned == 1);
    }
}
