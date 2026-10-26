/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * DMO upper MAC, receive side: EN 300 396-3 clauses 8.5 and 9.1 - 9.3.
 */

#include <string.h>
#include <tetra/tetra_coding.h>
#include "dmo_int.h"

/* Address type value meaning "no address". */
#define ADDR_NONE 2

static void emit_encrypted(struct tetra_dmo_rx *rx, uint8_t type)
{
    struct tetra_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_PDU;
    ev.u.pdu.proto = TETRA_PROTO_DMCC;
    ev.u.pdu.type = type;
    ev.u.pdu.encrypted = true;
    dmo_emit(rx, &ev);
}

/*
 * Common part of the DMAC-SYNC (SCH/H) and DMAC-DATA headers, from the
 * destination address type up to and including the message type.
 */
static bool parse_addr(struct bitrd *b, struct tetra_dmo_hdr *h,
                       uint32_t default_src)
{
    h->dst_type = (uint8_t)br_get(b, 2);
    h->dst = h->dst_type != ADDR_NONE ? br_get(b, 24) : 0;
    h->src_type = (uint8_t)br_get(b, 2);
    h->src = h->src_type != ADDR_NONE ? br_get(b, 24) : default_src;
    /* MNI is present for direct MS-MS and repeater operation */
    if (h->comm_type <= 1)
        h->mni = br_get(b, 24);
    h->msg_type = (uint8_t)br_get(b, 5);
    return !b->err;
}

void dmo_frag_abort(struct tetra_dmo_rx *rx)
{
    struct tetra_dmo_frag *f = &rx->frag;

    if (!f->active)
        return;
    f->active = false;
    /* EN 300 396-3 8.5.4.2 case 2: report incomplete short data */
    if (f->hdr.msg_type == DM_SDS_DATA || f->hdr.msg_type == DM_SDS_UDATA)
        dmo_l3(rx, &f->hdr, f->buf, 0, f->nbits, false);
}

static void frag_start(struct tetra_dmo_rx *rx, const struct tetra_dmo_hdr *h,
                       const uint8_t *buf, unsigned s, unsigned e)
{
    struct tetra_dmo_frag *f = &rx->frag;

    if (e - s > TETRA_DMO_MAX_SDU_BITS)
        return;
    f->active = true;
    f->started = false;
    f->hdr = *h;
    f->remaining = h->nfrag;
    f->dcc = tetra_dmo_colour_code(h->mni, h->src);
    bits_copy(f->buf, 0, buf, s, e - s);
    f->nbits = (uint16_t)(e - s);
}

static bool frag_append(struct tetra_dmo_rx *rx, const uint8_t *buf, unsigned s,
                        unsigned e)
{
    struct tetra_dmo_frag *f = &rx->frag;

    if (f->nbits + (e - s) > TETRA_DMO_MAX_SDU_BITS) {
        dmo_frag_abort(rx);
        return false;
    }
    bits_copy(f->buf, f->nbits, buf, s, e - s);
    f->nbits = (uint16_t)(f->nbits + (e - s));
    f->started = true;
    return true;
}

/* DMAC-SYNC, tables 21 and 22. pdu holds SCH/S + SCH/H (184 bits). */
void dmo_mac_sync(struct tetra_dmo_rx *rx, const uint8_t *pdu)
{
    struct bitrd b;
    struct tetra_dmo_hdr h;
    unsigned fill, slot, frame, end = 184;

    memset(&h, 0, sizeof(h));
    br_init(&b, pdu, 0, 184);
    br_skip(&b, 4); /* system code */
    if (br_get(&b, 2) != 0)
        return; /* DPRES-SYNC: repeater / gateway presence */
    h.comm_type = (uint8_t)br_get(&b, 2);
    br_skip(&b, 2 + 2); /* flags, A/B channel usage */
    slot = br_get(&b, 2);
    frame = br_get(&b, 5);
    h.aie = (uint8_t)br_get(&b, 2);
    br_skip(&b, 39);

    if (frame >= 1 && frame <= 18) {
        rx->t.tn = (uint8_t)(slot + 1);
        rx->t.fn = (uint8_t)frame;
    }

    /* SCH/H part */
    br_skip(&b, 10);
    fill = br_get(&b, 1);
    h.frag = br_get(&b, 1);
    if (h.frag)
        h.nfrag = (uint8_t)br_get(&b, 4);
    h.frame_countdown = (uint8_t)br_get(&b, 2);

    if (h.comm_type >= 2)
        return; /* DM-GATE operation is not supported */
    if (h.aie == 1) {
        /* DM-2-C: addressing and message type are encrypted */
        emit_encrypted(rx, 0xff);
        return;
    }
    if (!parse_addr(&b, &h, 0))
        return;
    if (fill)
        end = bits_strip_fill(pdu, b.pos, end);
    if (end < b.pos)
        return;

    unsigned msg_start = b.pos - 5;

    if (h.frag) {
        struct tetra_dmo_frag *f = &rx->frag;

        /* Repetitions of the heading DMAC-SYNC are ignored. */
        if (f->active && !f->started && f->hdr.src == h.src &&
            f->hdr.dst == h.dst && f->hdr.msg_type == h.msg_type) {
            f->hdr.frame_countdown = h.frame_countdown;
            return;
        }
        dmo_frag_abort(rx);
        frag_start(rx, &h, pdu, msg_start, end);
        return;
    }
    dmo_l3(rx, &h, pdu, msg_start, end, true);
}

/* DMAC-DATA, table 23. */
static int mac_data(struct tetra_dmo_rx *rx, enum dmo_lchan ch,
                    const uint8_t *blk, unsigned nbits)
{
    struct bitrd b;
    struct tetra_dmo_hdr h;
    unsigned fill, half2, end = nbits;
    int flags = 0;

    memset(&h, 0, sizeof(h));
    br_init(&b, blk, 2, nbits);
    fill = br_get(&b, 1);
    half2 = br_get(&b, 1);
    h.frag = br_get(&b, 1);
    if (ch == DMO_STCH1 && half2)
        flags |= DMO_MAC_HALF2_STOLEN;

    /* Any DMAC-DATA on SCH/F aborts a fragmented message. */
    if (ch == DMO_SCH_F)
        dmo_frag_abort(rx);

    if (br_get(&b, 1))
        return flags; /* null PDU */
    h.frame_countdown = (uint8_t)br_get(&b, 2);
    h.aie = (uint8_t)br_get(&b, 2);
    h.comm_type = 0;
    if (h.aie == 1) {
        emit_encrypted(rx, 0xff);
        return flags;
    }
    if (!parse_addr(&b, &h, rx->master_ssi))
        return flags;
    if (fill)
        end = bits_strip_fill(blk, b.pos, end);
    if (end < b.pos)
        return flags;

    unsigned msg_start = b.pos - 5;

    if (h.frag && ch == DMO_STCH1 && half2) {
        /* DMAC-DATA + DMAC-END in one stolen slot */
        dmo_frag_abort(rx);
        frag_start(rx, &h, blk, msg_start, end);
        rx->frag.dcc = rx->dcc;
        rx->frag.started = true;
        return flags;
    }
    dmo_l3(rx, &h, blk, msg_start, end, true);
    return flags;
}

/* DMAC-FRAG / DMAC-END, tables 24 and 25. */
static void mac_frag_end(struct tetra_dmo_rx *rx, const uint8_t *blk,
                         unsigned nbits)
{
    struct tetra_dmo_frag *f = &rx->frag;
    unsigned end_pdu = bit_at(blk, 2);
    unsigned fill = bit_at(blk, 3);
    unsigned end = fill ? bits_strip_fill(blk, 4, nbits) : nbits;

    if (!f->active)
        return;
    if (!frag_append(rx, blk, 4, end))
        return;
    if (end_pdu) {
        f->active = false;
        dmo_l3(rx, &f->hdr, f->buf, 0, f->nbits, true);
    } else if (f->remaining) {
        f->remaining--;
    }
}

int dmo_mac_dnb(struct tetra_dmo_rx *rx, enum dmo_lchan ch, const uint8_t *blk,
                unsigned nbits, bool crc_ok)
{
    unsigned type;

    if (!crc_ok) {
        /* EN 300 396-3 8.5.4.2 case 2 and 8.6.5.3 */
        if ((ch == DMO_SCH_F || ch == DMO_STCH2) && rx->frag.active)
            dmo_frag_abort(rx);
        return 0;
    }

    type = (bit_at(blk, 0) << 1) | bit_at(blk, 1);
    switch (type) {
    case 0:
        return mac_data(rx, ch, blk, nbits);
    case 1:
        if (ch != DMO_STCH1)
            mac_frag_end(rx, blk, nbits);
        return 0;
    case 3:
        /* DMAC-U-SIGNAL: U-plane signalling for the application */
        if (ch == DMO_STCH1 && bit_at(blk, 2))
            return DMO_MAC_HALF2_STOLEN;
        return 0;
    default:
        return 0;
    }
}
