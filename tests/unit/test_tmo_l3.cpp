/* SPDX-License-Identifier: GPL-3.0-or-later */

/* TMO layer 3: MLE network broadcast, MM and CMCE call PDUs. */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

#include <tetra/tetra.h>

extern "C" {
#include "tmo_int.h"
}

namespace
{

struct capture {
    struct tetra_tmo_rx rx;
    std::vector<struct tetra_call> calls;
    std::vector<struct tetra_mm> mm;
    std::vector<struct tetra_network> nwrk;
    unsigned pdus = 0;
};

void on_event(void *user, const struct tetra_event *ev)
{
    capture *c = static_cast<capture *>(user);

    switch (ev->type) {
    case TETRA_EV_CALL:
        c->calls.push_back(ev->u.call);
        break;
    case TETRA_EV_MM:
        c->mm.push_back(*ev->u.mm);
        break;
    case TETRA_EV_NETWORK:
        c->nwrk.push_back(*ev->u.network);
        break;
    case TETRA_EV_PDU:
        c->pdus++;
        break;
    default:
        break;
    }
}

void init(capture &c, uint32_t issi = 0)
{
    struct tetra_rx_config cfg;

    std::memset(&cfg, 0, sizeof(cfg));
    cfg.cb = on_event;
    cfg.user = &c;
    cfg.issi = issi;
    cfg.promiscuous = issi == 0;
    tetra_tmo_init(&c.rx, &cfg);
}

/* LLC BL-UDATA + MLE protocol discriminator + PDU bits. */
struct sdu {
    uint8_t b[128] = { 0 };
    unsigned n = 0;

    explicit sdu(unsigned pd)
    {
        put(2, 4);
        put(pd, 3);
    }
    sdu &put(uint32_t v, unsigned k)
    {
        bw_put(b, &n, v, k);
        return *this;
    }
    sdu &bits(const char *s)
    {
        for (; *s; s++)
            put(*s == '1', 1);
        return *this;
    }
};

void deliver(capture &c, const sdu &s, uint32_t ssi)
{
    struct tmo_macinfo mi;

    std::memset(&mi, 0, sizeof(mi));
    mi.tn = 1;
    mi.addr_type = TMO_ADDR_SSI;
    mi.ssi = ssi;
    tmo_llc(&c.rx, &mi, s.b, 0, s.n);
}

/* MM PDUs captured from the MorganBTS recording (after the MM PD). */
const char *mm_lu_accept =
    "0101011110000010011100010000110100000101010000011101000110111000001001"
    "100000010111000000000000001000110010100100";
const char *mm_group_ack =
    "1011101101110010010100000101010000000000000001101111100110100000000000"
    "0000110111110011110000000000000001101111101000100000000000000011011111"
    "0100110000000000000001101111101010100000000000000011011111010111000000"
    "0000000001101111101100100000000000000011011111011011000000000000000110"
    "1111101110100000000000000011011111011110";

} // namespace

TEST_CASE("MM location update accept from a real network", "[tmo][mm]")
{
    static capture c;

    init(c);
    deliver(c, sdu(TETRA_PROTO_MM).bits(mm_lu_accept), 320026);
    REQUIRE(c.mm.size() == 1);
    const struct tetra_mm &m = c.mm[0];
    REQUIRE(m.type == TETRA_MM_LOC_UPDATE_ACCEPT);
    REQUIRE(m.ssi == 320026);
    REQUIRE(m.lu_type == 3); /* ITSI attach */
    REQUIRE(m.new_ssi == 320026);
    REQUIRE_FALSE(m.group_reject);
    REQUIRE(m.ngroups == 1);
    REQUIRE_FALSE(m.groups[0].detach);
    REQUIRE(m.groups[0].gssi == 9001);
    REQUIRE(m.groups[0].lifetime == 3);
    REQUIRE(m.groups[0].usage == 4);
    REQUIRE(c.pdus == 0);
}

TEST_CASE("MM group detach acknowledgement from a real network", "[tmo][mm]")
{
    static capture c;

    init(c);
    deliver(c, sdu(TETRA_PROTO_MM).bits(mm_group_ack), 320026);
    REQUIRE(c.mm.size() == 1);
    const struct tetra_mm &m = c.mm[0];
    REQUIRE(m.type == TETRA_MM_GROUP_ATTACH_DETACH_ACK);
    REQUIRE(m.group_reject);
    REQUIRE(m.ngroups == 10);
    REQUIRE_FALSE(m.groups_truncated);
    for (unsigned i = 0; i < 10; i++) {
        REQUIRE(m.groups[i].detach);
        REQUIRE(m.groups[i].gssi == 7142 + i);
    }
}

TEST_CASE("MM reject, proceeding and addressing", "[tmo][mm]")
{
    static capture c;

    init(c, 1234);
    /* D-LOCATION UPDATE REJECT: roaming, LA not allowed, no ciphering */
    deliver(c, sdu(TETRA_PROTO_MM).put(7, 4).put(0, 3).put(3, 5).put(0, 2),
            1234);
    /* D-LOCATION UPDATE PROCEEDING to another MS: filtered */
    deliver(
        c,
        sdu(TETRA_PROTO_MM).put(9, 4).put(4321, 24).put(0x123456, 24).put(0, 1),
        4321);
    /* D-ATTACH/DETACH GROUP IDENTITY, detach all and attach GTSI 77 */
    deliver(c,
            sdu(TETRA_PROTO_MM)
                .put(10, 4)
                .put(0, 1)
                .put(0, 1)
                .put(1, 1) /* detach all */
                .put(1, 1) /* O-bit */
                .put(1, 1) /* M-bit */
                .put(7, 4)
                .put(6 + 1 + 5 + 2 + 48, 11)
                .put(1, 6)
                .put(0, 1)
                .put(1, 2)
                .put(2, 3)
                .put(1, 2) /* GSSI + extension */
                .put(77, 24)
                .put(0x0abcde, 24)
                .put(0, 1),
            1234);
    REQUIRE(c.mm.size() == 2);
    REQUIRE(c.mm[0].type == TETRA_MM_LOC_UPDATE_REJECT);
    REQUIRE(c.mm[0].cause == 3);
    REQUIRE(c.mm[0].addressed);
    REQUIRE(c.mm[1].type == TETRA_MM_GROUP_ATTACH_DETACH);
    REQUIRE(c.mm[1].detach_all);
    REQUIRE(c.mm[1].ngroups == 1);
    REQUIRE(c.mm[1].groups[0].gssi == 77);
    REQUIRE(c.mm[1].groups[0].ext == 0x0abcde);
    REQUIRE(c.mm[1].groups[0].usage == 2);
}

TEST_CASE("D-NWRK-BROADCAST with time and neighbour cells", "[tmo][mle]")
{
    static capture c;
    sdu s(TETRA_PROTO_MLE);

    s.put(MLE_D_NWRK_BROADCAST, 3).put(0xaa65, 16).put(2, 2);
    s.put(1, 1);                                     /* O-bit */
    s.put(1, 1).put(0x00a000, 24);                   /* network time: UTC */
    s.put(1, 1).put(4, 6).put(26, 6).put(0x7ff, 11); /* -1 h, 2026 */
    s.put(1, 1).put(2, 3);                           /* two neighbour cells */
    /* cell 3 with carrier extension, LA and TDMA frame offset */
    s.put(3, 5).put(2, 2).put(1, 1).put(1, 2).put(1234, 12);
    s.put(1, 1);                                         /* O-bit */
    s.put(1, 1).put(3, 4).put(1, 2).put(0, 3).put(0, 1); /* extension */
    s.put(0, 1).put(0, 1);                               /* MCC, MNC */
    s.put(1, 1).put(7, 14);                              /* LA */
    s.put(0, 5);                                         /* not present */
    s.put(1, 1).put(5, 6);                               /* frame offset */
    /* cell 4, mandatory part only */
    s.put(4, 5).put(0, 2).put(0, 1).put(3, 2).put(1300, 12).put(0, 1);

    init(c);
    deliver(c, s, 0xffffff);
    REQUIRE(c.nwrk.size() == 1);
    const struct tetra_network &n = c.nwrk[0];
    REQUIRE(n.reselect == 0xaa65);
    REQUIRE(n.load == 2);
    REQUIRE(n.has_time);
    REQUIRE(n.time_valid);
    REQUIRE(n.year == 2026);
    REQUIRE(n.utc_s == 2 * 0x00a000);
    REQUIRE(n.offset_valid);
    REQUIRE(n.offset_min == -60);
    REQUIRE(n.nneighbours == 2);
    REQUIRE(n.neighbour[0].cell_id == 3);
    REQUIRE(n.neighbour[0].synchronised);
    REQUIRE(n.neighbour[0].has_ext);
    REQUIRE(n.neighbour[0].dl_freq_hz == 330856250u);
    REQUIRE(n.neighbour[0].mcc == 0xffff);
    REQUIRE(n.neighbour[0].la == 7);
    REQUIRE(n.neighbour[0].tdma_offset == 5);
    REQUIRE(n.neighbour[1].cell_id == 4);
    REQUIRE(n.neighbour[1].load == 3);
    REQUIRE(n.neighbour[1].main_carrier == 1300);
    REQUIRE(n.neighbour[1].dl_freq_hz == 0); /* band not known yet */
    REQUIRE(n.neighbour[1].tdma_offset == 0xff);
    REQUIRE(tetra_tmo_network(&c.rx) != nullptr);
}

TEST_CASE("CMCE call set-up and floor control PDUs", "[tmo][cmce]")
{
    static capture c;
    const uint32_t caller = 555, group = 9001, other = 777;

    init(c);
    /* D-CALL PROCEEDING to the caller, then D-ALERT (queued) */
    deliver(c,
            sdu(TETRA_PROTO_CMCE)
                .put(1, 5)
                .put(100, 14)
                .put(0, 5)
                .put(1, 1)
                .put(0, 1)
                .put(1, 1)
                .put(0, 3)
                .put(0, 1),
            caller);
    deliver(c,
            sdu(TETRA_PROTO_CMCE)
                .put(0, 5)
                .put(100, 14)
                .put(0, 5)
                .put(1, 1)
                .put(0, 1),
            caller);
    /* repetition of the D-ALERT: not reported again */
    deliver(c,
            sdu(TETRA_PROTO_CMCE)
                .put(0, 5)
                .put(100, 14)
                .put(0, 5)
                .put(0, 1)
                .put(0, 1),
            caller);
    /* D-SETUP to the group, transmission granted to the caller */
    deliver(c,
            sdu(TETRA_PROTO_CMCE)
                .put(7, 5)
                .put(100, 14)
                .put(0, 4 + 1 + 1)
                .put(0x04, 8) /* point to multipoint */
                .put(0, 2)
                .put(0, 1)
                .put(0, 4)
                .put(1, 1)
                .put(0, 1)
                .put(0, 1)
                .put(1, 1)
                .put(1, 2)
                .put(caller, 24)
                .put(0, 1),
            group);
    /* D-TX WAIT, D-TX CONTINUE, D-TX INTERRUPT to another party */
    deliver(c, sdu(TETRA_PROTO_CMCE).put(12, 5).put(100, 14).put(0, 2), group);
    deliver(c,
            sdu(TETRA_PROTO_CMCE).put(10, 5).put(100, 14).put(1, 1).put(0, 2),
            group);
    deliver(c,
            sdu(TETRA_PROTO_CMCE)
                .put(13, 5)
                .put(100, 14)
                .put(3, 2)
                .put(0, 3)
                .put(1, 1)
                .put(0, 1)
                .put(1, 1)
                .put(1, 2)
                .put(other, 24)
                .put(0, 1),
            group);
    /* D-INFO renumbers the call, D-CALL RESTORE, D-TX CONTINUE (not) */
    deliver(c,
            sdu(TETRA_PROTO_CMCE)
                .put(5, 5)
                .put(100, 14)
                .put(0, 2)
                .put(1, 1)
                .put(1, 1)
                .put(101, 14)
                .put(0, 4)
                .put(1, 1)
                .put(3, 3)
                .put(0, 4),
            group);
    deliver(c,
            sdu(TETRA_PROTO_CMCE).put(14, 5).put(101, 14).put(0, 4).put(0, 1),
            group);
    deliver(c, sdu(TETRA_PROTO_CMCE).put(10, 5).put(101, 14).put(0, 3), group);

    std::vector<enum tetra_call_what> what;
    for (const auto &k : c.calls)
        what.push_back(k.what);
    const std::vector<enum tetra_call_what> expect = {
        TETRA_CALL_PROCEEDING, TETRA_CALL_ALERT,       TETRA_CALL_SETUP,
        TETRA_CALL_TX_WAIT,    TETRA_CALL_TX_CONTINUE, TETRA_CALL_TX_INTERRUPT,
        TETRA_CALL_INFO,       TETRA_CALL_RESTORE,     TETRA_CALL_TX_CEASED,
    };
    REQUIRE(what == expect);
    REQUIRE(c.calls[0].src_ssi == caller);
    REQUIRE(c.calls[0].status == TETRA_CALL_ST_PROCEEDING);
    REQUIRE(c.calls[1].status == TETRA_CALL_ST_QUEUED);
    REQUIRE(c.calls[2].dst_ssi == group);
    REQUIRE(c.calls[2].group);
    REQUIRE(c.calls[3].src_ssi == caller);
    REQUIRE(c.calls[5].src_ssi == other);
    REQUIRE(c.calls[6].call_id == 101);
    REQUIRE(c.calls[6].status == TETRA_CALL_ST_CONTINUE);
    REQUIRE(c.calls[7].call_id == 101);
    REQUIRE(c.calls[8].src_ssi == other);
}
