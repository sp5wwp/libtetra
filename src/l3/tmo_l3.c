/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * TMO LLC (basic link), MLE routing, CMCE downlink and uplink PDUs and
 * carrier following.
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

/* CMCE uplink PDU types (PDU type element, clause 14.8). */
enum {
    U_ALERT = 0,
    U_CONNECT = 2,
    U_DISCONNECT = 4,
    U_RELEASE = 6,
    U_SETUP = 7,
    U_STATUS = 8,
    U_TX_CEASED = 9,
    U_TX_DEMAND = 10,
    U_SDS_DATA = 15,
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

/* ---- Carrier following (tetra_rx_config.retune) ---- */

static bool retune(struct tetra_tmo_rx *rx, const struct tetra_retune *r)
{
    return rx->cfg.retune && r->freq_hz && rx->cfg.retune(rx->cfg.user, r);
}

/* Back to the main carrier, unless already there. */
static void follow_main(struct tetra_tmo_rx *rx)
{
    struct tetra_tmo_follow *f = &rx->follow;
    const struct tetra_cell *cell = &rx->cell;
    struct tetra_retune r;

    f->active = false;
    if (!cell->have_sysinfo || f->tuned == cell->main_carrier)
        return;
    memset(&r, 0, sizeof(r));
    r.reason = TETRA_RETUNE_MAIN;
    r.freq_hz = cell->dl_freq_hz;
    r.ul_freq_hz = cell->ul_freq_hz;
    r.carrier = cell->main_carrier;
    if (retune(rx, &r))
        f->tuned = cell->main_carrier;
}

static void follow_end(struct tetra_tmo_rx *rx,
                       const struct tetra_tmo_call_state *c)
{
    if (rx->follow.active && rx->follow.call_id == c->call_id)
        follow_main(rx);
}

void tmo_follow_sysinfo(struct tetra_tmo_rx *rx)
{
    /* SYSINFO is broadcast on the main carrier. */
    if (!rx->follow.active)
        rx->follow.tuned = rx->cell.main_carrier;
}

void tmo_follow_lost(struct tetra_tmo_rx *rx)
{
    rx->follow.pend = NULL;
    if (rx->follow.active || (rx->follow.tuned != 0xffff &&
                              rx->follow.tuned != rx->cell.main_carrier))
        follow_main(rx);
}

/* Downlink channel allocation for call c, followed after the TM-SDU. */
static void note_alloc(struct tetra_tmo_rx *rx, struct tetra_tmo_call_state *c,
                       const struct tmo_chan_alloc *ca)
{
    const struct tetra_cell *cell = &rx->cell;
    struct tetra_tmo_follow *f = &rx->follow;
    uint8_t band = cell->band, offset = cell->offset;
    uint8_t duplex = cell->duplex_spacing;
    bool reverse = cell->reverse_operation;

    if (!rx->cfg.retune || ca->ul_dl == 2)
        return; /* nothing to do, or uplink only */
    if (ca->has_ext) {
        band = ca->band;
        offset = ca->offset;
        duplex = ca->duplex_spacing;
        reverse = ca->reverse_operation;
    } else if (!cell->have_sysinfo) {
        return; /* band not known yet */
    }
    f->pend = c;
    f->pend_carrier = ca->carrier;
    f->pend_hz = tetra_carrier_hz(band, ca->carrier, offset);
    f->pend_ul_hz = tetra_uplink_hz(f->pend_hz, band, duplex, reverse);
}

static void follow_check(struct tetra_tmo_rx *rx)
{
    struct tetra_tmo_follow *f = &rx->follow;
    struct tetra_tmo_call_state *c = f->pend;
    struct tetra_retune r;

    f->pend = NULL;
    if (!c || !c->used || f->pend_carrier == f->tuned)
        return;
    if (f->active && f->call_id != c->call_id)
        return; /* busy following another call */
    memset(&r, 0, sizeof(r));
    r.reason = TETRA_RETUNE_CALL;
    r.freq_hz = f->pend_hz;
    r.ul_freq_hz = f->pend_ul_hz;
    r.carrier = f->pend_carrier;
    r.call_id = c->call_id;
    r.dst_ssi = c->dst_ssi;
    r.timeslots = c->timeslots;
    r.addressed = tmo_addressed(rx, c->dst_ssi) ||
                  (rx->cfg.issi && c->talker_ssi == rx->cfg.issi);
    if (!r.addressed && !rx->cfg.promiscuous)
        return;
    if (!retune(rx, &r))
        return;
    f->tuned = f->pend_carrier;
    f->call_id = c->call_id;
    f->active =
        !rx->cell.have_sysinfo || f->pend_carrier != rx->cell.main_carrier;
}

static void emit_call(struct tetra_tmo_rx *rx,
                      const struct tetra_tmo_call_state *c,
                      enum tetra_call_what what, uint8_t cause);

/*
 * A usage marker is valid only on the channel it was assigned with: a
 * carrier, its timeslots and a direction (EN 300 392-2 clause 23.5.5). The
 * same value can name other calls on other channels at the same time.
 * Traffic goes to the call holding its marker on the channel received
 * (tmo_traffic()), and that traffic keeps the call from expiring: a call
 * whose release went unheard, its marker then given to a new call on the
 * same channel, took the new call's traffic, its talker and its encryption
 * for as long as the marker carried traffic. Release the calls still
 * holding a marker given to another call on the same channel.
 */
static void um_take(struct tetra_tmo_rx *rx, struct tetra_tmo_call_state *c,
                    uint8_t um, const struct tmo_chan_alloc *ca)
{
    for (unsigned i = 0; i < TETRA_TMO_MAX_CALLS; i++) {
        struct tetra_tmo_call_state *o = &rx->calls[i];

        if (o == c || !o->used || o->usage_marker != um)
            continue;
        /* Only where both channels are known to be the same one */
        if (o->carrier != ca->carrier || !(o->timeslots & ca->timeslots))
            continue;
        emit_call(rx, o, TETRA_CALL_RELEASE, 0xff);
        o->used = false;
        follow_end(rx, o);
    }
}

static void call_apply_mac(struct tetra_tmo_rx *rx,
                           struct tetra_tmo_call_state *c,
                           const struct tmo_macinfo *mi)
{
    /* An uplink-only allocation does not change the downlink marker */
    if (mi->usage_marker && !(mi->has_ca && mi->ca.ul_dl == 2)) {
        if (mi->has_ca)
            um_take(rx, c, mi->usage_marker, &mi->ca);
        c->usage_marker = mi->usage_marker;
    }
    if (mi->has_ca) {
        c->timeslots = mi->ca.timeslots;
        c->carrier = mi->ca.carrier;
        note_alloc(rx, c, &mi->ca);
    }
    if (tmo_macinfo_encrypted(mi)) {
        c->encrypted = true;
#if TETRA_ENABLE_CRYPTO
        c->encryption_mode = mi->encryption_mode;
        c->encryption_mode_valid = true;
#endif
    }
    c->idle_frames = 0;
}

/* Basic service information: circuit mode type and encryption flag. */
static void call_apply_bsi(struct tetra_tmo_call_state *c, unsigned bsi)
{
    c->circuit_type = (uint8_t)(bsi >> 5);
    c->encrypted = c->encrypted || ((bsi >> 4) & 1);
}

/* Events reported once per call (tetra_tmo_call_state.seen). */
#define SEEN_SETUP 1
#define SEEN_ALERT 2
#define SEEN_PROCEEDING 4

static void emit_call_st(struct tetra_tmo_rx *rx,
                         const struct tetra_tmo_call_state *c,
                         enum tetra_call_what what, uint8_t cause,
                         uint8_t status)
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
    k->status = status;
    k->addressed = tmo_addressed(rx, c->dst_ssi) ||
                   (rx->cfg.issi && c->talker_ssi == rx->cfg.issi);
    if (!k->addressed && !rx->cfg.promiscuous)
        return;
    tmo_emit(rx, &ev);
}

static void emit_call(struct tetra_tmo_rx *rx,
                      const struct tetra_tmo_call_state *c,
                      enum tetra_call_what what, uint8_t cause)
{
    emit_call_st(rx, c, what, cause, TETRA_CALL_ST_NONE);
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
    call_apply_mac(rx, c, mi);
    c->dst_ssi = mi->ssi;
    c->group = comm != 0;
    call_apply_bsi(c, bsi);
    c->priority = (uint8_t)prio;
    if (calling && (grant == 0 || grant == 3)) {
        c->talker_ssi = calling;
        c->tx_active = true;
    }
    if (created || !(c->seen & SEEN_SETUP)) {
        c->seen |= SEEN_SETUP;
        emit_call(rx, c, TETRA_CALL_SETUP, 0);
    }
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
    call_apply_mac(rx, c, mi);
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
    call_apply_mac(rx, c, mi);
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
    call_apply_mac(rx, c, mi);
    if (!c->tx_active)
        return; /* repetition */
    emit_call(rx, c, TETRA_CALL_TX_CEASED, 0);
    c->talker_ssi = 0;
    c->tx_active = false;
}

/* D-ALERT and D-CALL PROCEEDING, tables 14.13 and 14.15. */
static void cmce_alert(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
                       struct bitrd *b, bool proceeding)
{
    struct tetra_tmo_call_state *c;
    struct opt o;
    bool created;
    uint16_t id = (uint16_t)br_get(b, 14);
    uint8_t status = TETRA_CALL_ST_NONE;
    unsigned queued, bsi = 0x100;
    uint8_t flag = proceeding ? SEEN_PROCEEDING : SEEN_ALERT;

    br_skip(b, 3 + 1 + 1); /* time-out, hook or reserved, simplex */
    queued = proceeding ? 0 : br_get(b, 1);
    opt_begin(&o, b);
    if (opt_p(&o))
        bsi = br_get(b, 8);
    if (proceeding && opt_p(&o))
        status = (uint8_t)br_get(b, 3);
    if (b->err)
        return;
    if (queued)
        status = TETRA_CALL_ST_QUEUED;

    /* Sent to the calling party, usually before the D-SETUP. */
    c = call_get(rx, id, &created);
    call_apply_mac(rx, c, mi);
    if (created)
        c->talker_ssi = mi->ssi;
    if (bsi < 0x100)
        call_apply_bsi(c, bsi);
    if ((c->seen & flag) && status == TETRA_CALL_ST_NONE)
        return; /* repetition */
    c->seen |= flag;
    emit_call_st(rx, c, proceeding ? TETRA_CALL_PROCEEDING : TETRA_CALL_ALERT,
                 0, status);
}

/* Optional new call identifier: the SwMI renumbers the call. */
static void call_renumber(struct tetra_tmo_rx *rx,
                          struct tetra_tmo_call_state *c, uint16_t id)
{
    struct tetra_tmo_call_state *old = call_find(rx, id);

    if (old && old != c)
        old->used = false;
    if (rx->follow.active && rx->follow.call_id == c->call_id)
        rx->follow.call_id = id;
    c->call_id = id;
}

/* D-INFO, table 14.20. */
static void cmce_info(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
                      struct bitrd *b)
{
    struct tetra_tmo_call_state *c;
    struct opt o;
    uint16_t id = (uint16_t)br_get(b, 14);
    uint32_t new_id = 0x10000;
    uint8_t status = TETRA_CALL_ST_NONE;

    br_skip(b, 1 + 1); /* reset time-out, poll request */
    opt_begin(&o, b);
    if (opt_p(&o))
        new_id = br_get(b, 14);
    if (opt_p(&o))
        br_skip(b, 4); /* call time-out */
    if (opt_p(&o))
        br_skip(b, 3); /* call time-out, set-up phase */
    if (opt_p(&o))
        br_skip(b, 1); /* call ownership */
    if (opt_p(&o))
        br_skip(b, 9); /* modify */
    if (opt_p(&o))
        status = (uint8_t)br_get(b, 3);
    c = call_find(rx, id);
    if (!c || b->err)
        return;
    call_apply_mac(rx, c, mi);
    if (new_id < 0x10000)
        call_renumber(rx, c, (uint16_t)new_id);
    emit_call_st(rx, c, TETRA_CALL_INFO, 0, status);
}

/* D-TX WAIT (table 14.30) and D-TX CONTINUE (table 14.26). */
static void cmce_tx_wait(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
                         struct bitrd *b, bool cont)
{
    struct tetra_tmo_call_state *c;
    uint16_t id = (uint16_t)br_get(b, 14);
    unsigned go = cont ? br_get(b, 1) : 0;

    c = call_find(rx, id);
    if (!c || b->err)
        return;
    call_apply_mac(rx, c, mi);
    if (cont && go) {
        if (c->tx_active)
            return; /* repetition */
        c->tx_active = true;
        emit_call(rx, c, TETRA_CALL_TX_CONTINUE, 0);
        return;
    }
    if (!c->tx_active)
        return; /* repetition, or not continued after a wait */
    emit_call(rx, c, cont ? TETRA_CALL_TX_CEASED : TETRA_CALL_TX_WAIT, 0);
    c->tx_active = false;
    if (cont)
        c->talker_ssi = 0;
}

/* D-TX INTERRUPT, table 14.29: another party gets the floor. */
static void cmce_tx_interrupt(struct tetra_tmo_rx *rx,
                              const struct tmo_macinfo *mi, struct bitrd *b)
{
    struct tetra_tmo_call_state *c;
    struct opt o;
    uint16_t id = (uint16_t)br_get(b, 14);
    unsigned grant = br_get(b, 2);
    uint32_t talker = 0;

    br_skip(b, 1 + 1 + 1);
    opt_begin(&o, b);
    if (opt_p(&o))
        br_skip(b, 6); /* notification indicator */
    if (opt_p(&o))
        talker = party_addr(b, br_get(b, 2), NULL);
    c = call_find(rx, id);
    if (!c || b->err)
        return;
    call_apply_mac(rx, c, mi);
    if (grant != 0 && grant != 3)
        return;
    if (c->tx_active && talker && c->talker_ssi == talker)
        return; /* repetition */
    c->talker_ssi = talker;
    c->tx_active = true;
    emit_call(rx, c, TETRA_CALL_TX_INTERRUPT, 0);
}

/* D-CALL RESTORE, table 14.16. */
static void cmce_restore(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
                         struct bitrd *b)
{
    struct tetra_tmo_call_state *c;
    struct opt o;
    bool created;
    uint16_t id = (uint16_t)br_get(b, 14);
    uint32_t new_id = 0x10000;
    uint8_t status = TETRA_CALL_ST_NONE;

    br_skip(b, 2 + 1 + 1); /* grant, request permission, reset time-out */
    opt_begin(&o, b);
    if (opt_p(&o))
        new_id = br_get(b, 14);
    if (opt_p(&o))
        br_skip(b, 4); /* call time-out */
    if (opt_p(&o))
        status = (uint8_t)br_get(b, 3);
    if (b->err)
        return;
    c = call_get(rx, id, &created);
    call_apply_mac(rx, c, mi);
    if (created)
        c->dst_ssi = mi->ssi;
    if (new_id < 0x10000)
        call_renumber(rx, c, (uint16_t)new_id);
    emit_call_st(rx, c, TETRA_CALL_RESTORE, 0, status);
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
    follow_end(rx, c);
}

static void emit_sds(struct tetra_tmo_rx *rx, struct tetra_sds *s)
{
    struct tetra_event ev;

    s->addressed =
        tmo_addressed(rx, s->dst_ssi) ||
        (rx->in_uplink && rx->cfg.issi && s->src_ssi == rx->cfg.issi);
    if (!s->addressed && !rx->cfg.promiscuous)
        return;
    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_SDS;
    ev.u.sds = *s;
    tmo_emit(rx, &ev);
}

/* Pre-coded status, or short data type identifier and user data. */
static void sds_body(struct tetra_tmo_rx *rx, struct tetra_sds *s,
                     struct bitrd *b, bool status)
{
    static const uint8_t udd_len[3] = { 16, 32, 64 };
    uint8_t data[(2047 + 7) / 8];
    unsigned n;

    s->complete = true;
    if (status) {
        s->type = TETRA_SDS_STATUS;
        s->status = (uint16_t)br_get(b, 16);
        if (!b->err)
            emit_sds(rx, s);
        return;
    }

    s->type = (enum tetra_sds_type)br_get(b, 2);
    n = s->type == TETRA_SDS_UDD4 ? br_get(b, 11) : udd_len[s->type];
    if (b->err || n > br_left(b))
        return;
    memset(data, 0, sizeof(data));
    bits_copy(data, 0, b->buf, b->pos, n);
    s->data = data;
    s->nbits = (uint16_t)n;
    emit_sds(rx, s);
}

static void cmce_sds(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
                     struct bitrd *b, bool status)
{
    struct tetra_sds s;

    memset(&s, 0, sizeof(s));
    s.dst_ssi = mi->ssi;
    s.src_ssi = party_addr(b, br_get(b, 2), &s.src_ext);
    sds_body(rx, &s, b, status);
}

/* Kept out of line so its stack does not add to the MLE and MM paths. */
static __attribute__((noinline)) void cmce_pdu(struct tetra_tmo_rx *rx,
                                               const struct tmo_macinfo *mi,
                                               const uint8_t *buf, unsigned s,
                                               unsigned e)
{
    struct bitrd b;
    unsigned type;

    br_init(&b, buf, s, e);
    type = br_get(&b, 5);

    switch (type) {
    case D_ALERT:
        cmce_alert(rx, mi, &b, false);
        break;
    case D_CALL_PROCEEDING:
        cmce_alert(rx, mi, &b, true);
        break;
    case D_INFO:
        cmce_info(rx, mi, &b);
        break;
    case D_TX_WAIT:
        cmce_tx_wait(rx, mi, &b, false);
        break;
    case D_TX_CONTINUE:
        cmce_tx_wait(rx, mi, &b, true);
        break;
    case D_TX_INTERRUPT:
        cmce_tx_interrupt(rx, mi, &b);
        break;
    case D_CALL_RESTORE:
        cmce_restore(rx, mi, &b);
        break;
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
    case TETRA_PROTO_SNDCP: {
        struct bitrd m = b;

        if (pd == TETRA_PROTO_MM && tmo_mm(rx, mi, &m))
            return;
        type = bit_at(buf, b.pos) << 3 | bit_at(buf, b.pos + 1) << 2 |
               bit_at(buf, b.pos + 2) << 1 | bit_at(buf, b.pos + 3);
        break;
    }
    case TETRA_PROTO_MLE:
        type = bit_at(buf, b.pos) << 2 | bit_at(buf, b.pos + 1) << 1 |
               bit_at(buf, b.pos + 2);
        if (type == MLE_D_NWRK_BROADCAST && br_left(&b) >= 3) {
            struct bitrd m = b;

            br_skip(&m, 3);
            if (tmo_mle_nwrk(rx, &m))
                return;
        }
        break;
    default:
        break;
    }
    if (br_left(&b) >= 4)
        emit_pdu(rx, (enum tetra_proto)pd, (uint8_t)type, mi->ssi, buf, b.pos,
                 e);
}

/*
 * LLC basic link header and FCS (clause 21.2.2), the same in both
 * directions. Advanced link PDUs are reported raw. Returns false if no
 * TL-SDU follows, else [*start, *end) is the MLE PDU.
 */
static bool llc_sdu(struct tetra_tmo_rx *rx, uint32_t addr, const uint8_t *buf,
                    unsigned *start, unsigned *end)
{
    struct bitrd b;
    unsigned type, e = *end;

    br_init(&b, buf, *start, e);
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
        emit_pdu(rx, TETRA_PROTO_LLC, (uint8_t)type, addr, buf, *start, e);
        return false;
    }
    if (type & 4) {
        unsigned fcs_pos;
        uint32_t fcs = 0;

        if (br_left(&b) < 32)
            return false;
        fcs_pos = e - 32;
        for (unsigned i = 0; i < 32; i++)
            fcs = (fcs << 1) | bit_at(buf, fcs_pos + i);
        if (tetra_fcs32_packed(buf, b.pos, fcs_pos - b.pos) != fcs)
            return false;
        e = fcs_pos;
    }
    if (b.err || b.pos >= e)
        return false;
    *start = b.pos;
    *end = e;
    return true;
}

void tmo_llc(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
             const uint8_t *buf, unsigned start, unsigned end)
{
    rx->follow.pend = NULL;
    if (end <= start) {
        /* MAC-only resource: update usage marker / channel allocation */
        for (unsigned i = 0; i < TETRA_TMO_MAX_CALLS; i++) {
            struct tetra_tmo_call_state *c = &rx->calls[i];

            if (c->used && c->dst_ssi == mi->ssi && mi->ssi)
                call_apply_mac(rx, c, mi);
        }
    } else if (llc_sdu(rx, mi->ssi, buf, &start, &end)) {
        mle_pdu(rx, mi, buf, start, end);
    }
    follow_check(rx);
}

/* ---- Traffic and timers ---- */

void tmo_traffic(struct tetra_tmo_rx *rx, uint8_t um, enum tetra_half h1,
                 enum tetra_half h2)
{
    struct tetra_event ev;
    struct tetra_traffic *t = &ev.u.traffic;
    struct tetra_tmo_call_state *c = NULL;
    /* The channel received: carrier and timeslot */
    uint16_t cn = rx->cell.main_carrier;
    const uint8_t ts = (rx->t.tn >= 1 && rx->t.tn <= 4)
                           ? (uint8_t)(1u << (rx->t.tn - 1)) : 0;
    int best = 0;

    if (rx->follow.active && rx->follow.tuned != 0xffff)
        cn = rx->follow.tuned;
    /* The call holding the marker on this channel; one whose channel is
     * not known yet if none is known to be on it */
    for (unsigned i = 0; i < TETRA_TMO_MAX_CALLS; i++) {
        struct tetra_tmo_call_state *o = &rx->calls[i];
        int score;

        if (!o->used || o->usage_marker != um)
            continue;
        if (o->carrier == 0xffff || o->timeslots == 0 || ts == 0)
            score = 1;
        else if (o->carrier == cn && (o->timeslots & ts))
            score = 2;
        else
            continue;
        if (score > best) {
            best = score;
            c = o;
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
#if TETRA_ENABLE_CRYPTO
        if (c->encrypted) {
            struct tetra_tmo_key_request request = {
                .cell = &rx->cell,
                .address = c->dst_ssi,
                .usage_marker = um,
                .traffic = true,
                .encryption_mode = c->encryption_mode,
                .encryption_mode_valid = c->encryption_mode_valid,
            };
            enum tetra_tea_algorithm algorithm;
            uint8_t eck[TETRA_TEA_KEY_BYTES];

            if (tmo_crypto_get_eck(rx, &request, &algorithm, eck) &&
                tmo_crypto_keystream_bits(rx, algorithm, eck, 0,
                                          TETRA_TCH_KSS_BITS,
                                          rx->traffic_keystream))
                t->cipher_keystream = rx->traffic_keystream;
            memset(eck, 0, sizeof(eck));
        }
#endif
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
            follow_end(rx, c);
        }
    }
}

/* ---- Uplink ---- */

#if TETRA_UPLINK_DECODE

/* Called party after its type identifier: SNA, SSI or TSI. */
static uint32_t ul_called(struct bitrd *b, uint32_t *ext)
{
    unsigned type = br_get(b, 2);

    if (type == 0) {
        br_skip(b, 8); /* short number address */
        return 0;
    }
    if (type == 3) {
        b->err = true;
        return 0;
    }
    return party_addr(b, type, ext);
}

static void emit_ul_call(struct tetra_tmo_rx *rx, struct tetra_call *k)
{
    struct tetra_event ev;

    k->carrier = 0xffff;
    k->status = TETRA_CALL_ST_NONE;
    k->addressed = tmo_addressed(rx, k->dst_ssi) ||
                   (rx->cfg.issi && k->src_ssi == rx->cfg.issi);
    if (!k->addressed && !rx->cfg.promiscuous)
        return;
    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_CALL;
    ev.u.call = *k;
    tmo_emit(rx, &ev);
}

/* CMCE uplink PDUs, clause 14.7. */
static __attribute__((noinline)) void ul_cmce(struct tetra_tmo_rx *rx,
                                              uint32_t ssi, const uint8_t *buf,
                                              unsigned s, unsigned e)
{
    struct bitrd b;
    struct tetra_call k;
    struct tetra_sds sds;
    unsigned type, bsi;

    br_init(&b, buf, s, e);
    type = br_get(&b, 5);
    memset(&k, 0, sizeof(k));
    k.src_ssi = ssi;

    switch (type) {
    case U_SETUP:
        br_skip(&b, 4 + 1 + 1); /* area selection, hook, simplex */
        bsi = br_get(&b, 8);
        br_skip(&b, 1); /* request to transmit */
        k.priority = (uint8_t)br_get(&b, 4);
        br_skip(&b, 2); /* CLIR control */
        k.dst_ssi = ul_called(&b, NULL);
        k.what = TETRA_CALL_SETUP;
        k.group = ((bsi >> 2) & 3) != 0;
        k.encrypted = (bsi >> 4) & 1;
        k.circuit_type = (uint8_t)(bsi >> 5);
        break;
    case U_TX_DEMAND:
        k.call_id = (uint16_t)br_get(&b, 14);
        k.priority = (uint8_t)br_get(&b, 2);
        k.encrypted = br_get(&b, 1);
        k.what = TETRA_CALL_TX_DEMAND;
        break;
    case U_TX_CEASED:
        k.call_id = (uint16_t)br_get(&b, 14);
        k.what = TETRA_CALL_TX_CEASED;
        break;
    case U_CONNECT:
        k.call_id = (uint16_t)br_get(&b, 14);
        k.what = TETRA_CALL_CONNECT;
        break;
    case U_ALERT:
        k.call_id = (uint16_t)br_get(&b, 14);
        k.what = TETRA_CALL_ALERT;
        break;
    case U_DISCONNECT:
    case U_RELEASE:
        k.call_id = (uint16_t)br_get(&b, 14);
        k.cause = (uint8_t)br_get(&b, 5);
        k.what = TETRA_CALL_RELEASE;
        break;
    case U_SDS_DATA:
    case U_STATUS:
        memset(&sds, 0, sizeof(sds));
        sds.src_ssi = ssi;
        br_skip(&b, 4); /* area selection */
        sds.dst_ssi = ul_called(&b, NULL);
        if (!b.err)
            sds_body(rx, &sds, &b, type == U_STATUS);
        return;
    default:
        emit_pdu(rx, TETRA_PROTO_CMCE, (uint8_t)type, ssi, buf, s, e);
        return;
    }
    if (!b.err)
        emit_ul_call(rx, &k);
}

void tmo_ul_llc(struct tetra_tmo_rx *rx, uint32_t ssi, const uint8_t *buf,
                unsigned start, unsigned end)
{
    struct bitrd b;
    unsigned pd, type;

    if (end <= start || !llc_sdu(rx, ssi, buf, &start, &end))
        return;
    br_init(&b, buf, start, end);
    pd = br_get(&b, 3);
    if (b.err)
        return;
    if (pd == TETRA_PROTO_CMCE) {
        ul_cmce(rx, ssi, buf, b.pos, end);
        return;
    }
    /* MM, SNDCP: 4-bit PDU type; MLE: 3 bits. Reported raw. */
    type = br_get(&b, pd == TETRA_PROTO_MLE ? 3 : 4);
    if (!b.err)
        emit_pdu(rx, (enum tetra_proto)pd, (uint8_t)type, ssi, buf,
                 b.pos - (pd == TETRA_PROTO_MLE ? 3 : 4), end);
}

void tmo_ul_encrypted(struct tetra_tmo_rx *rx, uint32_t ssi)
{
    struct tetra_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_PDU;
    ev.u.pdu.proto = TETRA_PROTO_LLC;
    ev.u.pdu.addr = ssi;
    ev.u.pdu.encrypted = true;
    tmo_emit(rx, &ev);
}

#endif /* TETRA_UPLINK_DECODE */
