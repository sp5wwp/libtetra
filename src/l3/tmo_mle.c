/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * TMO MLE network broadcast and MM downlink PDUs.
 * EN 300 392-2 clauses 16.9.2, 16.10, 18.4.1.4.1, 18.5 and annex E.
 */

#include <string.h>
#include "tmo_int.h"

/* ---- D-NWRK-BROADCAST ---- */

/* TETRA network time element, table 18.100. */
static void network_time(struct tetra_network *n, struct bitrd *b)
{
    uint32_t utc = br_get(b, 24);
    unsigned sign = br_get(b, 1), off = br_get(b, 6);
    unsigned year = br_get(b, 6);

    br_skip(b, 11);
    n->has_time = true;
    n->time_valid = utc < 0xf142ff && year < 0x3f;
    n->utc_s = utc * 2;
    n->year = (uint16_t)(2000 + year);
    n->offset_valid = off <= 56;
    n->offset_min = (int16_t)((sign ? -1 : 1) * (int)off * 15);
}

/* Neighbour cell information for CA, table 18.64 and annex E.1.4. */
static void neighbour(const struct tetra_tmo_rx *rx, struct tetra_neighbour *c,
                      struct bitrd *b)
{
    struct opt o;

    memset(c, 0, sizeof(*c));
    c->mcc = c->mnc = c->la = 0xffff;
    c->tdma_offset = 0xff;
    c->cell_id = (uint8_t)br_get(b, 5);
    c->reselection = (uint8_t)br_get(b, 2);
    c->synchronised = br_get(b, 1);
    c->load = (uint8_t)br_get(b, 2);
    c->main_carrier = (uint16_t)br_get(b, 12);

    opt_begin(&o, b);
    if (opt_p(&o)) {
        c->has_ext = true;
        c->band = (uint8_t)br_get(b, 4);
        c->offset = (uint8_t)br_get(b, 2);
        c->duplex_spacing = (uint8_t)br_get(b, 3);
        c->reverse_operation = br_get(b, 1);
    }
    if (opt_p(&o))
        c->mcc = (uint16_t)br_get(b, 10);
    if (opt_p(&o))
        c->mnc = (uint16_t)br_get(b, 14);
    if (opt_p(&o))
        c->la = (uint16_t)br_get(b, 14);
    if (opt_p(&o))
        br_skip(b, 3); /* maximum MS transmit power */
    if (opt_p(&o))
        br_skip(b, 4); /* minimum RX access level */
    if (opt_p(&o))
        br_skip(b, 16); /* subscriber class */
    if (opt_p(&o))
        br_skip(b, 12); /* BS service details */
    if (opt_p(&o))
        br_skip(b, 5); /* timeshare cell information */
    if (opt_p(&o))
        c->tdma_offset = (uint8_t)br_get(b, 6);

    if (c->has_ext)
        c->dl_freq_hz = tetra_carrier_hz(c->band, c->main_carrier, c->offset);
    else if (rx->cell.have_sysinfo)
        c->dl_freq_hz =
            tetra_carrier_hz(rx->cell.band, c->main_carrier, rx->cell.offset);
}

bool tmo_mle_nwrk(struct tetra_tmo_rx *rx, struct bitrd *b)
{
    struct tetra_network *n = &rx->network;
    struct tetra_event ev;
    struct opt o;

    /* Decoded in place to save stack: a broken PDU clears the data. */
    memset(n, 0, sizeof(*n));
    rx->have_network = false;
    n->reselect = (uint16_t)br_get(b, 16);
    n->load = (uint8_t)br_get(b, 2);
    opt_begin(&o, b);
    if (opt_p(&o))
        network_time(n, b);
    if (opt_p(&o)) {
        unsigned cnt = br_get(b, 3);

        for (unsigned i = 0; i < cnt && !b->err; i++)
            neighbour(rx, &n->neighbour[i], b);
        n->nneighbours = (uint8_t)cnt;
    }
    if (b->err)
        return false;

    rx->have_network = true;
    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_NETWORK;
    ev.u.network = &rx->network;
    tmo_emit(rx, &ev);
    return true;
}

const struct tetra_network *tetra_tmo_network(const struct tetra_tmo_rx *rx)
{
    return rx->have_network ? &rx->network : NULL;
}

/* ---- MM ---- */

/* Downlink MM PDU types, table 16.75. */
enum {
    D_LOC_UPDATE_ACCEPT = 5,
    D_LOC_UPDATE_COMMAND = 6,
    D_LOC_UPDATE_REJECT = 7,
    D_LOC_UPDATE_PROCEEDING = 9,
    D_ATTACH_DETACH_GROUP = 10,
    D_ATTACH_DETACH_GROUP_ACK = 11,
    D_MM_STATUS = 12,
};

/* Downlink type 3/4 element identifiers, table 16.95. */
enum {
    T34_NEW_REGISTERED_AREA = 2,
    T34_GROUP_LOCATION_ACCEPT = 5,
    T34_GROUP_DOWNLINK = 7,
};

/* Group identity downlink (type 4), table 16.54. */
static void group_downlink(struct tetra_mm *m, struct bitrd *b)
{
    unsigned cnt = br_get(b, 6);

    for (unsigned i = 0; i < cnt && !b->err; i++) {
        struct tetra_mm_group g;
        unsigned at;

        memset(&g, 0, sizeof(g));
        g.detach = br_get(b, 1);
        if (!g.detach) {
            g.lifetime = (uint8_t)br_get(b, 2);
            g.usage = (uint8_t)br_get(b, 3);
        } else {
            g.usage = (uint8_t)br_get(b, 2);
        }
        at = br_get(b, 2);
        if (at != 2)
            g.gssi = br_get(b, 24);
        if (at & 1)
            g.ext = br_get(b, 24);
        if (at >= 2)
            g.vgssi = br_get(b, 24);
        if (b->err)
            break;
        if (m->ngroups < TETRA_MM_MAX_GROUPS)
            m->groups[m->ngroups++] = g;
        else
            m->groups_truncated = true;
    }
}

/* New registered area (type 4), table 16.76: report the first LA. */
static void new_area(struct tetra_mm *m, struct bitrd *b)
{
    unsigned cnt = br_get(b, 6);

    for (unsigned i = 0; i < cnt && !b->err; i++) {
        struct opt o;
        uint16_t la;

        br_skip(b, 3); /* LA timer */
        la = (uint16_t)br_get(b, 14);
        opt_begin(&o, b);
        if (opt_p(&o))
            br_skip(b, 10); /* LACC */
        if (opt_p(&o))
            br_skip(b, 14); /* LANC */
        if (!b->err && m->la == 0xffff)
            m->la = la;
    }
}

/* Type 3/4 elements of an MM PDU or of a group identity location accept. */
static void mm_elements(struct tetra_mm *m, struct bitrd *b)
{
    struct bitrd e;
    unsigned id;

    while (t34_next(b, &id, &e)) {
        switch (id) {
        case T34_NEW_REGISTERED_AREA:
            new_area(m, &e);
            break;
        case T34_GROUP_DOWNLINK:
            group_downlink(m, &e);
            break;
        case T34_GROUP_LOCATION_ACCEPT: {
            struct opt o;

            m->group_reject = br_get(&e, 1);
            br_skip(&e, 1);
            opt_begin(&o, &e);
            if (o.o)
                mm_elements(m, &e);
            break;
        }
        default:
            break;
        }
    }
}

/* Cipher control and the ciphering parameters conditional on it. */
static void skip_cipher(struct bitrd *b)
{
    if (br_get(b, 1))
        br_skip(b, 10);
}

bool tmo_mm(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
            struct bitrd *b)
{
    struct tetra_mm m;
    struct tetra_event ev;
    struct opt o;

    memset(&m, 0, sizeof(m));
    m.ssi = mi->ssi;
    m.la = 0xffff;

    switch (br_get(b, 4)) {
    case D_LOC_UPDATE_ACCEPT:
        m.type = TETRA_MM_LOC_UPDATE_ACCEPT;
        m.lu_type = (uint8_t)br_get(b, 3);
        opt_begin(&o, b);
        if (opt_p(&o))
            m.new_ssi = br_get(b, 24);
        if (opt_p(&o))
            m.mni = br_get(b, 24);
        if (opt_p(&o))
            br_skip(b, 16); /* subscriber class */
        if (opt_p(&o))
            br_skip(b, 14); /* energy saving information */
        if (opt_p(&o))
            br_skip(b, 6); /* SCCH information */
        if (o.o)
            mm_elements(&m, b);
        break;
    case D_LOC_UPDATE_REJECT:
        m.type = TETRA_MM_LOC_UPDATE_REJECT;
        m.lu_type = (uint8_t)br_get(b, 3);
        m.cause = (uint8_t)br_get(b, 5);
        skip_cipher(b);
        opt_begin(&o, b);
        if (opt_p(&o))
            m.mni = br_get(b, 24);
        break;
    case D_LOC_UPDATE_COMMAND:
        m.type = TETRA_MM_LOC_UPDATE_COMMAND;
        m.report_req = br_get(b, 1);
        skip_cipher(b);
        opt_begin(&o, b);
        if (opt_p(&o))
            m.mni = br_get(b, 24);
        break;
    case D_LOC_UPDATE_PROCEEDING:
        m.type = TETRA_MM_LOC_UPDATE_PROCEEDING;
        m.new_ssi = br_get(b, 24);
        m.mni = br_get(b, 24);
        break;
    case D_ATTACH_DETACH_GROUP:
        m.type = TETRA_MM_GROUP_ATTACH_DETACH;
        m.report_req = br_get(b, 1);
        br_skip(b, 1); /* acknowledgement request */
        m.detach_all = br_get(b, 1);
        opt_begin(&o, b);
        if (o.o)
            mm_elements(&m, b);
        break;
    case D_ATTACH_DETACH_GROUP_ACK:
        m.type = TETRA_MM_GROUP_ATTACH_DETACH_ACK;
        m.group_reject = br_get(b, 1);
        br_skip(b, 1);
        opt_begin(&o, b);
        if (o.o)
            mm_elements(&m, b);
        break;
    case D_MM_STATUS:
        m.type = TETRA_MM_STATUS;
        m.status = (uint8_t)br_get(b, 6);
        break;
    default:
        return false;
    }
    if (b->err)
        return false;

    m.addressed = rx->cfg.issi && m.ssi == rx->cfg.issi;
    if (!m.addressed && !rx->cfg.promiscuous)
        return true;
    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_MM;
    ev.u.mm = &m;
    tmo_emit(rx, &ev);
    return true;
}
