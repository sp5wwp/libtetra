/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * DMO layer 3 receive side: DMCC messages, short data and the channel /
 * call state seen by a listening DM-MS. EN 300 396-3 clauses 6, 8.4, 8.6,
 * 9.4 - 9.7.
 */

#include <string.h>
#include <tetra/tetra_coding.h>
#include "dmo_int.h"

/* DT207 / DT221 default: 90 frames. */
#define DT_INACTIVITY 90

/* Cause reported when a call ends without a release message. */
#define CAUSE_LOST 0xff
#define CAUSE_RESERVATION_END 0xfe

static void set_chan(struct tetra_dmo_rx *rx, enum tetra_dmo_chan_state st,
                     uint32_t master, uint32_t mni)
{
    struct tetra_event ev;

    if (rx->chan == st && rx->master_ssi == master)
        return;
    rx->chan = st;
    rx->master_ssi = master;
    rx->master_mni = mni;
    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_CHANNEL;
    ev.u.channel.state = st;
    ev.u.channel.master_ssi = master;
    ev.u.channel.mni = mni;
    ev.u.channel.priority = rx->priority;
    dmo_emit(rx, &ev);
}

static void emit_call(struct tetra_dmo_rx *rx, enum tetra_call_what what,
                      uint8_t cause)
{
    struct tetra_event ev;
    struct tetra_call *k = &ev.u.call;

    if (!rx->call_addressed && !rx->cfg.promiscuous)
        return;
    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_CALL;
    k->what = what;
    k->carrier = 0xffff;
    k->dst_ssi = rx->call_dst;
    k->src_ssi = rx->talker;
    k->mni = rx->master_mni;
    k->group = rx->call_group;
    k->encrypted = rx->call_encrypted;
    k->addressed = rx->call_addressed;
    k->priority = rx->priority;
    k->circuit_type = rx->circuit_type;
    k->cause = cause;
    k->status = TETRA_CALL_ST_NONE;
    dmo_emit(rx, &ev);
}

static void end_call(struct tetra_dmo_rx *rx, uint8_t cause)
{
    if (rx->in_call)
        emit_call(rx, TETRA_CALL_RELEASE, cause);
    rx->in_call = false;
    rx->traffic = false;
    rx->talker = 0;
}

void dmo_lost(struct tetra_dmo_rx *rx)
{
    dmo_frag_abort(rx);
    end_call(rx, CAUSE_LOST);
    rx->have_dcc = false;
    set_chan(rx, TETRA_DMO_FREE, 0, 0);
}

static void emit_pdu(struct tetra_dmo_rx *rx, const struct tetra_dmo_hdr *h,
                     const uint8_t *buf, unsigned s, unsigned e)
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
    ev.u.pdu.proto = TETRA_PROTO_DMCC;
    ev.u.pdu.type = h->msg_type;
    ev.u.pdu.addr = h->dst;
    ev.u.pdu.encrypted = h->aie != 0;
    ev.u.pdu.data = tmp;
    ev.u.pdu.nbits = (uint16_t)n;
    dmo_emit(rx, &ev);
}

/* Duplicate suppression with the frame countdown, 8.5.6.2 a). */
static bool is_duplicate(struct tetra_dmo_rx *rx, const struct tetra_dmo_hdr *h)
{
    if (rx->last.valid && rx->last.msg_type == h->msg_type &&
        rx->last.src == h->src && rx->last.dst == h->dst)
        return true;
    rx->last.valid = true;
    rx->last.msg_type = h->msg_type;
    rx->last.src = h->src;
    rx->last.dst = h->dst;
    rx->last.frames_left = (uint8_t)(h->frame_countdown + 1);
    return false;
}

/* Elements of DM-SETUP, DM-CONNECT ACK and DM-OCCUPIED (tables 31-36). */
static void circuit_msg(struct tetra_dmo_rx *rx, const struct tetra_dmo_hdr *h,
                        struct bitrd *b)
{
    bool addressed = dmo_addressed(rx, h->dst);
    bool new_call = !rx->in_call || rx->call_dst != h->dst;
    unsigned circuit, prio, e2e = 0, group = 0;

    br_skip(b, 11);
    circuit = br_get(b, 4);
    br_skip(b, 4);
    prio = br_get(b, 2);
    if (h->aie == 0) {
        e2e = br_get(b, 1);
        group = br_get(b, 1);
    }
    if (b->err)
        return;

    /* A new transaction starts with DM-SETUP from another master or
     * after a pause; DM-OCCUPIED only refreshes the ongoing one. */
    bool new_tx = (h->msg_type == DM_SETUP &&
                   (h->src != rx->master_ssi || !rx->traffic)) ||
                  (h->msg_type == DM_CONNECT_ACK && !rx->traffic);

    rx->priority = (uint8_t)prio;
    rx->frames_no_occupied = 0;
    rx->dcc = tetra_dmo_colour_code(h->mni, h->src);
    rx->have_dcc = true;
    set_chan(rx, TETRA_DMO_OCCUPIED, h->src, h->mni);

    if (new_call) {
        rx->in_call = true;
        rx->call_dst = h->dst;
        rx->call_addressed = addressed;
        rx->call_group = group;
        rx->circuit_type = (uint8_t)circuit;
        rx->call_encrypted = e2e || h->aie != 0;
        rx->talker = h->src;
        emit_call(rx,
                  h->msg_type == DM_OCCUPIED ? TETRA_CALL_LATE_ENTRY
                                             : TETRA_CALL_SETUP,
                  0);
    } else if (new_tx) {
        /* call continuation after changeover */
        rx->talker = h->src;
        rx->call_encrypted = e2e || h->aie != 0;
        emit_call(rx,
                  h->msg_type == DM_CONNECT_ACK ? TETRA_CALL_CONNECT
                                                : TETRA_CALL_TX_GRANTED,
                  0);
    }
    if (addressed || rx->cfg.promiscuous)
        rx->traffic = true;
}

static void tx_ceased(struct tetra_dmo_rx *rx, const struct tetra_dmo_hdr *h,
                      struct bitrd *b)
{
    unsigned cause = 0;

    br_skip(b, 6 + 1);
    if (br_get(b, 1))
        br_skip(b, 1 + 8);
    else
        br_skip(b, 1);
    br_skip(b, 1);
    if (br_get(b, 1))
        br_skip(b, 12);
    rx->priority = (uint8_t)br_get(b, 2);
    if (h->aie == 0)
        cause = br_get(b, 4);
    set_chan(rx, TETRA_DMO_RESERVED, h->src, h->mni);
    if (rx->in_call && rx->traffic) {
        emit_call(rx, TETRA_CALL_TX_CEASED, (uint8_t)cause);
        rx->talker = 0;
    }
    rx->traffic = false;
}

static void reserved(struct tetra_dmo_rx *rx, const struct tetra_dmo_hdr *h,
                     struct bitrd *b)
{
    unsigned remaining;

    br_skip(b, 1);
    remaining = br_get(b, 6);
    if (b->err)
        return;
    rx->traffic = false;
    if (!rx->in_call) {
        /* late entry into a call in reservation */
        rx->in_call = true;
        rx->call_dst = h->dst;
        rx->call_addressed = dmo_addressed(rx, h->dst);
        rx->talker = 0;
    }
    rx->frames_no_occupied = 0;
    if (remaining == 0 && (rx->t.fn == 6 || rx->t.fn == 12 || rx->t.fn == 18)) {
        end_call(rx, CAUSE_RESERVATION_END);
        set_chan(rx, TETRA_DMO_FREE, 0, 0);
        return;
    }
    set_chan(rx, TETRA_DMO_RESERVED, h->src, h->mni);
}

static void dm_info(struct tetra_dmo_rx *rx, const struct tetra_dmo_hdr *h,
                    struct bitrd *b)
{
    if (h->aie != 0 || br_get(b, 3) != 0)
        return;
    br_skip(b, 24); /* MNI part of the TSI */
    uint32_t ssi = br_get(b, 24);

    if (b->err || !rx->in_call || ssi == rx->talker)
        return;
    rx->talker = ssi;
    emit_call(rx, TETRA_CALL_INFO, 0);
}

static void sds(struct tetra_dmo_rx *rx, const struct tetra_dmo_hdr *h,
                struct bitrd *b, bool complete)
{
    static const uint8_t udd_len[5] = { 16, 32, 64, 0, 16 };
    struct tetra_event ev;
    struct tetra_sds *s = &ev.u.sds;
    uint8_t data[(2047 + 7) / 8];
    unsigned fcs_flag, sdu_start, sdti, n;

    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_SDS;
    s->src_ssi = h->src;
    s->dst_ssi = h->dst;
    s->addressed = dmo_addressed(rx, h->dst);
    s->complete = complete;

    br_skip(b, 4); /* SDS time remaining */
    if (br_get(b, 1) == 0 && rx->chan != TETRA_DMO_OCCUPIED)
        set_chan(rx, TETRA_DMO_SDS_OCCUPIED, h->src, h->mni);
    br_skip(b, 2);
    fcs_flag = br_get(b, 1);
    s->has_fcs = fcs_flag;

    if (!s->addressed && !rx->cfg.promiscuous)
        return;
    if (h->aie != 0) {
        emit_pdu(rx, h, b->buf, b->pos, b->end);
        return;
    }
    if (!complete) {
        dmo_emit(rx, &ev);
        return;
    }

    sdu_start = b->pos;
    if (br_get(b, 1)) {
        if (br_get(b, 4) == 1) {
            s->src_ext = br_get(b, 24);
            s->src_ssi = br_get(b, 24);
        }
    }
    sdti = br_get(b, 4);
    if (sdti > 4 || b->err)
        return;
    s->type = (enum tetra_sds_type)sdti;
    n = sdti == 3 ? br_get(b, 11) : udd_len[sdti];
    if (b->err || n > br_left(b))
        return;
    memset(data, 0, sizeof(data));
    bits_copy(data, 0, b->buf, b->pos, n);
    br_skip(b, n);
    if (sdti == 4) {
        s->status = (uint16_t)(data[0] << 8 | data[1]);
    } else {
        s->data = data;
        s->nbits = (uint16_t)n;
    }
    if (fcs_flag) {
        uint32_t fcs;
        unsigned fcs_pos = b->pos;

        fcs = br_get(b, 32);
        s->fcs_ok = !b->err && tetra_fcs32_packed(b->buf, sdu_start,
                                                  fcs_pos - sdu_start) == fcs;
    }
    dmo_emit(rx, &ev);
}

void dmo_l3(struct tetra_dmo_rx *rx, const struct tetra_dmo_hdr *h,
            const uint8_t *buf, unsigned start, unsigned end, bool complete)
{
    struct bitrd b;

    br_init(&b, buf, start + 5, end);
    rx->frames_idle = 0;

    switch (h->msg_type) {
    case DM_OCCUPIED:
    case DM_RESERVED:
    case DM_SDS_OCCUPIED:
        break; /* periodic, handled idempotently */
    default:
        if (complete && is_duplicate(rx, h))
            return;
        break;
    }

    switch (h->msg_type) {
    case DM_SETUP:
    case DM_CONNECT_ACK:
    case DM_OCCUPIED:
        circuit_msg(rx, h, &b);
        break;
    case DM_SETUP_PRES:
        rx->in_call = true;
        rx->call_dst = h->dst;
        rx->call_addressed = dmo_addressed(rx, h->dst);
        rx->call_group = false;
        rx->talker = h->src;
        rx->dcc = tetra_dmo_colour_code(h->mni, h->src);
        rx->have_dcc = true;
        set_chan(rx, TETRA_DMO_OCCUPIED, h->src, h->mni);
        emit_call(rx, TETRA_CALL_SETUP, 0);
        break;
    case DM_RELEASE: {
        unsigned cause = h->aie == 0 ? br_get(&b, 4) : 0;

        end_call(rx, (uint8_t)cause);
        set_chan(rx, TETRA_DMO_FREE, 0, 0);
        break;
    }
    case DM_TX_CEASED:
        tx_ceased(rx, h, &b);
        break;
    case DM_RESERVED:
        reserved(rx, h, &b);
        break;
    case DM_SDS_OCCUPIED:
        if (rx->chan != TETRA_DMO_OCCUPIED)
            set_chan(rx, TETRA_DMO_SDS_OCCUPIED, h->src, h->mni);
        break;
    case DM_INFO:
        dm_info(rx, h, &b);
        break;
    case DM_SDS_UDATA:
    case DM_SDS_DATA:
        sds(rx, h, &b, complete);
        break;
    default:
        emit_pdu(rx, h, buf, start, end);
        break;
    }
}

void dmo_traffic(struct tetra_dmo_rx *rx, enum tetra_half h1,
                 enum tetra_half h2)
{
    struct tetra_event ev;
    struct tetra_traffic *t = &ev.u.traffic;

    rx->frames_no_occupied = 0;
    if (!rx->call_addressed && !rx->cfg.promiscuous)
        return;
    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_TRAFFIC;
    t->time = rx->t;
    t->time.mn = 0;
    t->talker_ssi = rx->talker;
    t->circuit_type = rx->circuit_type;
    t->encrypted = rx->call_encrypted;
    t->half[0] = h1;
    t->half[1] = h2;
    t->bits = rx->tch;
    dmo_emit(rx, &ev);
}

void dmo_frame_tick(struct tetra_dmo_rx *rx)
{
    if (rx->last.valid && --rx->last.frames_left == 0)
        rx->last.valid = false;

    if (rx->frames_idle < 0xffff)
        rx->frames_idle++;
    if (rx->frames_no_occupied < 0xffff)
        rx->frames_no_occupied++;

    /* DT221: no occupation message during traffic */
    if (rx->traffic && rx->frames_no_occupied > DT_INACTIVITY) {
        end_call(rx, CAUSE_LOST);
        set_chan(rx, TETRA_DMO_FREE, 0, 0);
    }
    /* DT207: no activity at all */
    if (rx->chan != TETRA_DMO_FREE && rx->frames_idle > DT_INACTIVITY) {
        end_call(rx, CAUSE_LOST);
        set_chan(rx, TETRA_DMO_FREE, 0, 0);
    }
}
