/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * TMO LLC (basic link), MLE routing and CMCE downlink PDUs.
 * EN 300 392-2 clauses 14.7, 14.8, 18.4, 21.2 and annex E.
 */

#include <string.h>
#include <tetra/tetra_coding.h>
#include "tmo_int.h"

/* CMCE downlink PDU types, table 14.66. */
enum {
    D_ALERT = 0,
    D_CALL_PROCEEDING = 1,
    D_CONNECT = 2,
    D_CONNECT_ACK = 3,
    D_DISCONNECT = 4,
    D_INFO = 5,
    D_RELEASE = 6,
    D_SETUP = 7,
    D_STATUS = 8,
    D_TX_CEASED = 9,
    D_TX_CONTINUE = 10,
    D_TX_GRANTED = 11,
    D_TX_WAIT = 12,
    D_TX_INTERRUPT = 13,
    D_CALL_RESTORE = 14,
    D_SDS_DATA = 15,
};

/* Calls idle longer than this (in frames, about 60 s) are dropped. */
#define CALL_IDLE_FRAMES (18 * 60)

static void emit_pdu(struct tetra_tmo_rx *rx, enum tetra_proto proto,
                     uint8_t type, uint32_t addr, const uint8_t *buf,
                     unsigned s, unsigned e)
{
    struct tetra_event ev;
    uint8_t tmp[64];
    unsigned n = e - s;

    if (n > sizeof(tmp) * 8)
        n = sizeof(tmp) * 8;
    memset(tmp, 0, sizeof(tmp));
    bits_copy(tmp, 0, buf, s, n);

    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_PDU;
    ev.u.pdu.proto = proto;
    ev.u.pdu.type = type;
    ev.u.pdu.addr = addr;
    ev.u.pdu.data = tmp;
    ev.u.pdu.nbits = (uint16_t)n;
    tmo_emit(rx, &ev);
}

/* ---- Call table ---- */

static struct tetra_tmo_call_state *call_find(struct tetra_tmo_rx *rx,
                                              uint16_t id)
{
    for (unsigned i = 0; i < TETRA_TMO_MAX_CALLS; i++) {
        if (rx->calls[i].used && rx->calls[i].call_id == id)
            return &rx->calls[i];
    }
    return NULL;
}

static struct tetra_tmo_call_state *call_get(struct tetra_tmo_rx *rx,
                                             uint16_t id, bool *created)
{
    struct tetra_tmo_call_state *c = call_find(rx, id);
    unsigned oldest = 0;

    *created = false;
    if (c)
        return c;
    for (unsigned i = 0; i < TETRA_TMO_MAX_CALLS; i++) {
        if (!rx->calls[i].used) {
            oldest = i;
            break;
        }
        if (rx->calls[i].idle_frames > rx->calls[oldest].idle_frames)
            oldest = i;
    }
    c = &rx->calls[oldest];
    memset(c, 0, sizeof(*c));
    c->used = true;
    c->call_id = id;
    c->carrier = 0xffff;
    *created = true;
    return c;
}

static void call_apply_mac(struct tetra_tmo_call_state *c,
                           const struct tmo_macinfo *mi)
{
    if (mi->usage_marker)
        c->usage_marker = mi->usage_marker;
    if (mi->has_ca) {
        c->timeslots = mi->ca.timeslots;
        c->carrier = mi->ca.carrier;
    }
    if (mi->enc)
        c->encrypted = true;
    c->idle_frames = 0;
}

static void emit_call(struct tetra_tmo_rx *rx,
                      const struct tetra_tmo_call_state *c,
                      enum tetra_call_what what, uint8_t cause)
{
    struct tetra_event ev;
    struct tetra_call *k = &ev.u.call;

    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_CALL;
    k->what = what;
    k->call_id = c->call_id;
    k->usage_marker = c->usage_marker;
    k->timeslots = c->timeslots;
    k->carrier = c->carrier;
    k->dst_ssi = c->dst_ssi;
    k->src_ssi = c->talker_ssi;
    k->group = c->group;
    k->encrypted = c->encrypted;
    k->priority = c->priority;
    k->circuit_type = c->circuit_type;
    k->cause = cause;
    k->addressed = tmo_addressed(rx, c->dst_ssi);
    if (!k->addressed && !rx->cfg.promiscuous)
        return;
    tmo_emit(rx, &ev);
}

/* ---- Type 2 / type 3 element helpers (annex E) ---- */

struct opt {
    struct bitrd *b;
    bool o;
};

static void opt_begin(struct opt *o, struct bitrd *b)
{
    o->b = b;
    o->o = br_left(b) ? br_get(b, 1) : false;
}

/* P-bit of the next type 2 element. */
static bool opt_p(struct opt *o)
{
    if (!o->o || !br_left(o->b))
        return false;
    return br_get(o->b, 1);
}

/* Party address after a type identifier: 1 = SSI, 2 = SSI + extension. */
static uint32_t party_addr(struct bitrd *b, unsigned type, uint32_t *ext)
{
    uint32_t ssi = 0;

    if (type == 1 || type == 2)
        ssi = br_get(b, 24);
    if (type == 2) {
        uint32_t e = br_get(b, 24);

        if (ext)
            *ext = e;
    }
    return ssi;
}

/* ---- CMCE PDUs ---- */

static void cmce_setup(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
                       struct bitrd *b)
{
    struct tetra_tmo_call_state *c;
    struct opt o;
    bool created;
    uint16_t id = (uint16_t)br_get(b, 14);
    unsigned bsi, grant, prio, comm;
    uint32_t calling = 0;

    br_skip(b, 4 + 1 + 1); /* time-out, hook, simplex */
    bsi = br_get(b, 8);
    grant = br_get(b, 2);
    br_skip(b, 1);
    prio = br_get(b, 4);
    opt_begin(&o, b);
    if (opt_p(&o))
        br_skip(b, 6); /* notification indicator */
    if (opt_p(&o))
        br_skip(b, 24); /* temporary address */
    if (opt_p(&o))
        calling = party_addr(b, br_get(b, 2), NULL);
    if (b->err)
        return;

    comm = (bsi >> 2) & 3;
    c = call_get(rx, id, &created);
    call_apply_mac(c, mi);
    c->dst_ssi = mi->ssi;
    c->group = comm != 0;
    c->circuit_type = (uint8_t)(bsi >> 5);
    c->encrypted = c->encrypted || ((bsi >> 4) & 1);
    c->priority = (uint8_t)prio;
    if (calling && (grant == 0 || grant == 3)) {
        c->talker_ssi = calling;
        c->tx_active = true;
    }
    if (created)
        emit_call(rx, c, TETRA_CALL_SETUP, 0);
}

static void cmce_connect(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
                         struct bitrd *b, bool ack)
{
    struct tetra_tmo_call_state *c;
    bool created;
    uint16_t id = (uint16_t)br_get(b, 14);

    if (b->err)
        return;
    c = call_get(rx, id, &created);
    call_apply_mac(c, mi);
    if (created)
        c->dst_ssi = mi->ssi;
    (void)ack;
    if (c->connected)
        return; /* repetition */
    c->connected = true;
    emit_call(rx, c, TETRA_CALL_CONNECT, 0);
}

static void cmce_tx_granted(struct tetra_tmo_rx *rx,
                            const struct tmo_macinfo *mi, struct bitrd *b)
{
    struct tetra_tmo_call_state *c;
    struct opt o;
    bool created;
    uint16_t id = (uint16_t)br_get(b, 14);
    unsigned grant = br_get(b, 2);
    uint32_t talker = 0;

    br_skip(b, 1 + 1 + 1);
    opt_begin(&o, b);
    if (opt_p(&o))
        br_skip(b, 6);
    if (opt_p(&o))
        talker = party_addr(b, br_get(b, 2), NULL);
    if (b->err)
        return;

    c = call_get(rx, id, &created);
    call_apply_mac(c, mi);
    if (created)
        c->dst_ssi = mi->ssi;
    if (grant != 0 && grant != 3)
        return; /* not granted / queued */
    if (!talker && grant == 0)
        talker = mi->ssi;
    if (c->tx_active && c->talker_ssi == talker)
        return; /* repetition */
    c->talker_ssi = talker;
    c->tx_active = true;
    emit_call(rx, c, TETRA_CALL_TX_GRANTED, 0);
}

static void cmce_tx_ceased(struct tetra_tmo_rx *rx,
                           const struct tmo_macinfo *mi, struct bitrd *b)
{
    struct tetra_tmo_call_state *c;
    uint16_t id = (uint16_t)br_get(b, 14);

    c = call_find(rx, id);
    if (!c || b->err)
        return;
    call_apply_mac(c, mi);
    if (!c->tx_active)
        return; /* repetition */
    emit_call(rx, c, TETRA_CALL_TX_CEASED, 0);
    c->talker_ssi = 0;
    c->tx_active = false;
}

static void cmce_release(struct tetra_tmo_rx *rx, struct bitrd *b)
{
    struct tetra_tmo_call_state *c;
    uint16_t id = (uint16_t)br_get(b, 14);
    uint8_t cause = (uint8_t)br_get(b, 5);

    c = call_find(rx, id);
    if (!c || b->err)
        return;
    emit_call(rx, c, TETRA_CALL_RELEASE, cause);
    c->used = false;
}

static void emit_sds(struct tetra_tmo_rx *rx, struct tetra_sds *s)
{
    struct tetra_event ev;

    s->addressed = tmo_addressed(rx, s->dst_ssi);
    if (!s->addressed && !rx->cfg.promiscuous)
        return;
    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_SDS;
    ev.u.sds = *s;
    tmo_emit(rx, &ev);
}

static void cmce_sds(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
                     struct bitrd *b, bool status)
{
    struct tetra_sds s;
    static const uint8_t udd_len[3] = { 16, 32, 64 };
    uint8_t data[(2047 + 7) / 8];
    unsigned n;

    memset(&s, 0, sizeof(s));
    s.dst_ssi = mi->ssi;
    s.src_ssi = party_addr(b, br_get(b, 2), &s.src_ext);
    s.complete = true;

    if (status) {
        s.type = TETRA_SDS_STATUS;
        s.status = (uint16_t)br_get(b, 16);
        if (!b->err)
            emit_sds(rx, &s);
        return;
    }

    s.type = (enum tetra_sds_type)br_get(b, 2);
    n = s.type == TETRA_SDS_UDD4 ? br_get(b, 11) : udd_len[s.type];
    if (b->err || n > br_left(b))
        return;
    memset(data, 0, sizeof(data));
    bits_copy(data, 0, b->buf, b->pos, n);
    s.data = data;
    s.nbits = (uint16_t)n;
    emit_sds(rx, &s);
}

static void cmce_pdu(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
                     const uint8_t *buf, unsigned s, unsigned e)
{
    struct bitrd b;
    unsigned type;

    br_init(&b, buf, s, e);
    type = br_get(&b, 5);

    switch (type) {
    case D_SETUP:
        cmce_setup(rx, mi, &b);
        break;
    case D_CONNECT:
        cmce_connect(rx, mi, &b, false);
        break;
    case D_CONNECT_ACK:
        cmce_connect(rx, mi, &b, true);
        break;
    case D_TX_GRANTED:
        cmce_tx_granted(rx, mi, &b);
        break;
    case D_TX_CEASED:
        cmce_tx_ceased(rx, mi, &b);
        break;
    case D_RELEASE:
    case D_DISCONNECT:
        cmce_release(rx, &b);
        break;
    case D_SDS_DATA:
        cmce_sds(rx, mi, &b, false);
        break;
    case D_STATUS:
        cmce_sds(rx, mi, &b, true);
        break;
    default:
        emit_pdu(rx, TETRA_PROTO_CMCE, (uint8_t)type, mi->ssi, buf, s, e);
        break;
    }
}

/* ---- MLE and LLC ---- */

static void mle_pdu(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
                    const uint8_t *buf, unsigned s, unsigned e)
{
    struct bitrd b;
    unsigned pd, type = 0;

    br_init(&b, buf, s, e);
    pd = br_get(&b, 3);
    if (b.err)
        return;

    switch (pd) {
    case TETRA_PROTO_CMCE:
        cmce_pdu(rx, mi, buf, b.pos, e);
        return;
    case TETRA_PROTO_MM:
    case TETRA_PROTO_SNDCP:
        type = bit_at(buf, b.pos) << 3 | bit_at(buf, b.pos + 1) << 2 |
               bit_at(buf, b.pos + 2) << 1 | bit_at(buf, b.pos + 3);
        break;
    case TETRA_PROTO_MLE:
        type = bit_at(buf, b.pos) << 2 | bit_at(buf, b.pos + 1) << 1 |
               bit_at(buf, b.pos + 2);
        break;
    default:
        break;
    }
    if (br_left(&b) >= 4)
        emit_pdu(rx, (enum tetra_proto)pd, (uint8_t)type, mi->ssi, buf, b.pos,
                 e);
}

void tmo_llc(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
             const uint8_t *buf, unsigned start, unsigned end)
{
    struct bitrd b;
    unsigned type;

    if (end <= start) {
        /* MAC-only resource: update usage marker / channel allocation */
        for (unsigned i = 0; i < TETRA_TMO_MAX_CALLS; i++) {
            struct tetra_tmo_call_state *c = &rx->calls[i];

            if (c->used && c->dst_ssi == mi->ssi && mi->ssi)
                call_apply_mac(c, mi);
        }
        return;
    }

    br_init(&b, buf, start, end);
    type = br_get(&b, 4);
    switch (type & 7) {
    case 0: /* BL-ADATA */
        br_skip(&b, 2);
        break;
    case 1: /* BL-DATA */
    case 3: /* BL-ACK */
        br_skip(&b, 1);
        break;
    case 2: /* BL-UDATA */
        break;
    }
    if (type >= 8) {
        emit_pdu(rx, TETRA_PROTO_LLC, (uint8_t)type, mi->ssi, buf, start, end);
        return;
    }
    if (type & 4) {
        unsigned fcs_pos;
        uint32_t fcs = 0;

        if (br_left(&b) < 32)
            return;
        fcs_pos = end - 32;
        for (unsigned i = 0; i < 32; i++)
            fcs = (fcs << 1) | bit_at(buf, fcs_pos + i);
        if (tetra_fcs32_packed(buf, b.pos, fcs_pos - b.pos) != fcs)
            return;
        end = fcs_pos;
    }
    if (b.err || b.pos >= end)
        return;
    mle_pdu(rx, mi, buf, b.pos, end);
}

/* ---- Traffic and timers ---- */

void tmo_traffic(struct tetra_tmo_rx *rx, uint8_t um, enum tetra_half h1,
                 enum tetra_half h2)
{
    struct tetra_event ev;
    struct tetra_traffic *t = &ev.u.traffic;
    struct tetra_tmo_call_state *c = NULL;

    for (unsigned i = 0; i < TETRA_TMO_MAX_CALLS; i++) {
        if (rx->calls[i].used && rx->calls[i].usage_marker == um) {
            c = &rx->calls[i];
            break;
        }
    }

    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_TRAFFIC;
    t->time = rx->t;
    t->usage_marker = um;
    t->half[0] = h1;
    t->half[1] = h2;
    t->bits = rx->tch;
    if (c) {
        c->idle_frames = 0;
        t->call_id = c->call_id;
        t->talker_ssi = c->talker_ssi;
        t->circuit_type = c->circuit_type;
        t->encrypted = c->encrypted;
        if (!rx->cfg.promiscuous && !tmo_addressed(rx, c->dst_ssi))
            return;
    } else if (!rx->cfg.promiscuous) {
        return;
    }
    tmo_emit(rx, &ev);
}

void tmo_frame_tick(struct tetra_tmo_rx *rx)
{
    for (unsigned i = 0; i < TETRA_TMO_MAX_CALLS; i++) {
        struct tetra_tmo_call_state *c = &rx->calls[i];

        if (!c->used)
            continue;
        if (++c->idle_frames >= CALL_IDLE_FRAMES) {
            emit_call(rx, c, TETRA_CALL_RELEASE, 0xff);
            c->used = false;
        }
    }
}
