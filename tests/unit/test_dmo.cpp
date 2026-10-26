/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

#include <tetra/tetra.h>
#include "../../tools/dmo_scenario.h"

namespace
{

struct traffic_rec {
    uint8_t fn;
    enum tetra_half half[2];
    std::vector<uint8_t> bits;
};

struct capture {
    struct tetra_dmo_rx rx;
    std::vector<struct tetra_call> calls;
    std::vector<struct tetra_sds> sds;
    std::vector<std::string> texts;
    std::vector<traffic_rec> traffic;
    std::vector<enum tetra_dmo_chan_state> chan;
    unsigned locks = 0;
    unsigned shift = 0; /* extra noise bits injected once */
    bool shifted = false;
};

void on_event(void *user, const struct tetra_event *ev)
{
    capture *c = static_cast<capture *>(user);

    switch (ev->type) {
    case TETRA_EV_SYNC:
        if (ev->u.sync.locked)
            c->locks++;
        break;
    case TETRA_EV_CALL:
        c->calls.push_back(ev->u.call);
        break;
    case TETRA_EV_CHANNEL:
        c->chan.push_back(ev->u.channel.state);
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
        traffic_rec r;

        r.fn = ev->time.fn;
        r.half[0] = ev->u.traffic.half[0];
        r.half[1] = ev->u.traffic.half[1];
        for (unsigned i = 0; i < TETRA_TCH_BITS; i++)
            r.bits.push_back(ev->u.traffic.bits[i] > 0);
        c->traffic.push_back(r);
        break;
    }
    default:
        break;
    }
}

void on_slot(void *user, const uint8_t bits[510], bool active)
{
    capture *c = static_cast<capture *>(user);

    (void)active;
    tetra_dmo_push_bits(&c->rx, bits, 510);
    /* Optionally shift the timing once, after the call. */
    if (c->shift && !c->shifted && c->chan.size() >= 4) {
        std::vector<uint8_t> noise(c->shift, 1);

        tetra_dmo_push_bits(&c->rx, noise.data(), noise.size());
        c->shifted = true;
    }
}

unsigned run(capture &c, const struct tetra_rx_config &cfg)
{
    struct dmo_scn s;
    struct tetra_rx_config k = cfg;

    k.cb = on_event;
    k.user = &c;
    tetra_dmo_init(&c.rx, &k);
    scn_init(&s, on_slot, &c, 0x1234567);
    return scn_run_reference(&s);
}

struct tetra_rx_config promisc()
{
    struct tetra_rx_config cfg;

    std::memset(&cfg, 0, sizeof(cfg));
    cfg.promiscuous = true;
    return cfg;
}

} // namespace

TEST_CASE("DMO receiver decodes the reference scenario", "[dmo]")
{
    static capture c;
    unsigned sent = run(c, promisc());

    REQUIRE(c.locks == 1);

    /* call control */
    REQUIRE(c.calls.size() == 4);
    REQUIRE(c.calls[0].what == TETRA_CALL_SETUP);
    REQUIRE(c.calls[0].dst_ssi == 1001);
    REQUIRE(c.calls[0].src_ssi == 2002);
    REQUIRE(c.calls[0].group);
    REQUIRE_FALSE(c.calls[0].encrypted);
    REQUIRE(c.calls[0].priority == 1);
    REQUIRE(c.calls[1].what == TETRA_CALL_INFO);
    REQUIRE(c.calls[1].src_ssi == SCN_TPNI_SSI);
    REQUIRE(c.calls[2].what == TETRA_CALL_TX_CEASED);
    REQUIRE(c.calls[2].cause == 1);
    REQUIRE(c.calls[3].what == TETRA_CALL_RELEASE);

    /* channel states */
    REQUIRE(c.chan.size() == 4);
    REQUIRE(c.chan[0] == TETRA_DMO_OCCUPIED);
    REQUIRE(c.chan[1] == TETRA_DMO_RESERVED);
    REQUIRE(c.chan[2] == TETRA_DMO_FREE);
    REQUIRE(c.chan[3] == TETRA_DMO_SDS_OCCUPIED);

    /* traffic: every slot, content intact */
    REQUIRE(c.traffic.size() == sent);
    unsigned stolen = 0;

    for (auto &t : c.traffic) {
        uint8_t ref[432];

        scn_tch_pattern(ref, t.fn);
        if (t.half[0] == TETRA_HALF_TCH) {
            REQUIRE(std::memcmp(t.bits.data(), ref, 432) == 0);
        } else {
            /* half-slot frame in block 2 */
            stolen++;
            REQUIRE(t.half[0] == TETRA_HALF_STOLEN_C);
            REQUIRE(std::memcmp(t.bits.data() + 216, ref + 216, 216) == 0);
        }
    }
    REQUIRE(stolen == 1);

    /* short data: status, then fragmented text with FCS */
    REQUIRE(c.sds.size() == 2);
    REQUIRE(c.sds[0].type == TETRA_SDS_STATUS);
    REQUIRE(c.sds[0].status == SCN_STATUS);
    REQUIRE(c.sds[0].src_ssi == 3003);
    REQUIRE(c.sds[0].dst_ssi == 2002);
    REQUIRE(c.sds[1].type == TETRA_SDS_UDD4);
    REQUIRE(c.sds[1].complete);
    REQUIRE(c.sds[1].has_fcs);
    REQUIRE(c.sds[1].fcs_ok);
    REQUIRE(c.texts.size() == 1);
    REQUIRE(c.texts[0] == SCN_TEXT);
}

TEST_CASE("DMO receiver follows a timing change", "[dmo]")
{
    static capture c;

    c.shift = 101; /* odd number of bits: also breaks dibit alignment */
    run(c, promisc());
    REQUIRE(c.shifted);
    REQUIRE(c.locks >= 1);
    REQUIRE(c.sds.size() == 2);
    REQUIRE(c.texts.size() == 1);
    REQUIRE(c.texts[0] == SCN_TEXT);
}

TEST_CASE("DMO receiver filters by address when not promiscuous", "[dmo]")
{
    static capture a, b;
    const uint32_t groups[1] = { 1001 };
    struct tetra_rx_config cfg;

    std::memset(&cfg, 0, sizeof(cfg));
    cfg.issi = 9999;
    cfg.groups = groups;
    cfg.ngroups = 1;
    run(a, cfg);
    REQUIRE(a.calls.size() == 4); /* member of group 1001 */
    REQUIRE_FALSE(a.traffic.empty());
    REQUIRE(a.sds.empty()); /* SDS is for 2002 */

    cfg.ngroups = 0;
    cfg.issi = 2002;
    run(b, cfg);
    REQUIRE(b.calls.empty());
    REQUIRE(b.traffic.empty());
    REQUIRE(b.sds.size() == 2);
}
