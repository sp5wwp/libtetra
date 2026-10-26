/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * TMO upper MAC, downlink: EN 300 392-2 clauses 21.4, 21.5 and 23.4.
 * Only π/4-DQPSK is handled (Y2 = Z2 = 1 octet).
 */

#include <string.h>
#include <tetra/tetra_coding.h>
#include "tmo_int.h"

#define LI_HALF2_STOLEN 0x3e
#define LI_FRAG_START 0x3f

static void emit_cell(struct tetra_tmo_rx *rx)
{
    struct tetra_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_CELL;
    ev.u.cell = &rx->cell;
    tmo_emit(rx, &ev);
}

/* SYNC PDU on the BSCH, table 21.76 + D-MLE-SYNC (table 18.17). */
void tmo_mac_sync(struct tetra_tmo_rx *rx, const uint8_t *blk)
{
    struct bitrd b;
    struct tetra_cell *c = &rx->cell;
    uint8_t syscode, cc, tn, fn, mn, share;
    uint16_t mcc, mnc;

    br_init(&b, blk, 0, 60);
    syscode = (uint8_t)br_get(&b, 4);
    cc = (uint8_t)br_get(&b, 6);
    tn = (uint8_t)br_get(&b, 2);
    fn = (uint8_t)br_get(&b, 5);
    mn = (uint8_t)br_get(&b, 6);
    share = (uint8_t)br_get(&b, 2);
    br_skip(&b, 3 + 1 + 1 + 1);
    mcc = (uint16_t)br_get(&b, 10);
    mnc = (uint16_t)br_get(&b, 14);

    if (fn < 1 || fn > 18 || mn < 1 || mn > 60)
        return;

    rx->t.tn = (uint8_t)(tn + 1);
    rx->t.fn = fn;
    rx->t.mn = mn;
    rx->scramb = tetra_scramb_init(tetra_tmo_colour_code(mcc, mnc, cc));

    if (c->mcc != mcc || c->mnc != mnc || c->colour_code != cc ||
        c->system_code != syscode || c->sharing_mode != share) {
        c->mcc = mcc;
        c->mnc = mnc;
        c->colour_code = cc;
        c->system_code = syscode;
        c->sharing_mode = share;
        c->have_sysinfo = false;
        emit_cell(rx);
    }
}

/* SYSINFO PDU, table 21.65 + D-MLE-SYSINFO (table 18.19). */
static void parse_sysinfo(struct tetra_tmo_rx *rx, struct bitrd *b)
{
    struct tetra_cell n = rx->cell;
    bool is_cck;
    uint16_t hf;

    n.main_carrier = (uint16_t)br_get(b, 12);
    n.band = (uint8_t)br_get(b, 4);
    n.offset = (uint8_t)br_get(b, 2);
    n.duplex_spacing = (uint8_t)br_get(b, 3);
    n.reverse_operation = br_get(b, 1);
    br_skip(b, 2 + 3 + 4 + 4 + 4);
    is_cck = br_get(b, 1);
    hf = (uint16_t)br_get(b, 16);
    br_skip(b, 2 + 20);
    n.location_area = (uint16_t)br_get(b, 14);
    br_skip(b, 16);
    n.service_details = (uint16_t)br_get(b, 12);
    if (b->err)
        return;
    n.air_encryption = (n.service_details >> 1) & 1;
    n.dl_freq_hz = tetra_carrier_hz(n.band, n.main_carrier, n.offset);
    n.have_sysinfo = true;

    /* Hyperframe and CCK alternate in the same field and change slowly:
     * they are updated but do not trigger an event on their own. */
    n.hyperframe = rx->cell.hyperframe;
    n.cck_id = rx->cell.cck_id;
    if (memcmp(&n, &rx->cell, sizeof(n))) {
        rx->cell = n;
        rx->cell.hyperframe = is_cck ? rx->cell.hyperframe : hf;
        rx->cell.cck_id = is_cck ? hf : rx->cell.cck_id;
        emit_cell(rx);
        return;
    }
    if (is_cck)
        rx->cell.cck_id = hf;
    else
        rx->cell.hyperframe = hf;
}

/* Channel allocation element, table 21.87. Returns false to discard. */
static bool parse_chan_alloc(struct bitrd *b, struct tmo_chan_alloc *ca)
{
    unsigned ts, mon, ud, mod, nap;

    ca->alloc_type = (uint8_t)br_get(b, 2);
    ts = br_get(b, 4);
    ud = br_get(b, 2);
    ca->ul_dl = (uint8_t)ud;
    br_skip(b, 1 + 1);
    ca->carrier = (uint16_t)br_get(b, 12);
    if (br_get(b, 1))
        br_skip(b, 4 + 2 + 3 + 1);
    mon = br_get(b, 2);
    if (mon == 0)
        br_skip(b, 2);
    if (ud == 0) {
        br_skip(b, 2 + 3);
        mod = br_get(b, 3);
        br_skip(b, 3); /* max UL QAM level or reserved */
        br_skip(b, 3 + 4 + 5);
        nap = br_get(b, 2);
        if (nap == 1)
            br_skip(b, 11);
        br_skip(b, 4);
        if (br_get(b, 1))
            br_skip(b, 16);
        if (br_get(b, 1))
            br_skip(b, 16);
        if (br_get(b, 1))
            return false;
        (void)mod;
    }

    /* Timeslot assigned: bit map with TN1 as MSB; 0 means MCCH. */
    ca->timeslots = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (ts & (8u >> i))
            ca->timeslots |= (uint8_t)(1u << i);
    }
    return !b->err;
}

/* PDU length in bits from the length indication, 0 if reserved. */
static unsigned li_bits(unsigned li)
{
    if (li >= 1 && li <= 0x3a)
        return li * 8;
    return 0;
}

static void frag_start(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
                       const uint8_t *blk, unsigned s, unsigned e)
{
    struct tetra_tmo_frag *f = &rx->frag[mi->tn - 1];

    f->active = true;
    f->enc = mi->enc;
    f->addr_type = mi->addr_type;
    f->addr = mi->ssi;
    f->usage_marker = mi->usage_marker;
    f->nbits = 0;
    if (e - s > TETRA_TMO_MAX_SDU_BITS) {
        f->active = false;
        return;
    }
    bits_copy(f->buf, 0, blk, s, e - s);
    f->nbits = (uint16_t)(e - s);
}

static bool frag_append(struct tetra_tmo_rx *rx, uint8_t tn, const uint8_t *blk,
                        unsigned s, unsigned e)
{
    struct tetra_tmo_frag *f = &rx->frag[tn - 1];

    if (!f->active)
        return false;
    if (f->nbits + (e - s) > TETRA_TMO_MAX_SDU_BITS) {
        f->active = false;
        return false;
    }
    bits_copy(f->buf, f->nbits, blk, s, e - s);
    f->nbits = (uint16_t)(f->nbits + (e - s));
    return true;
}

static void deliver(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
                    const uint8_t *buf, unsigned s, unsigned e)
{
    if (e <= s)
        return;
    if (mi->enc) {
        struct tetra_event ev;

        memset(&ev, 0, sizeof(ev));
        ev.type = TETRA_EV_PDU;
        ev.u.pdu.proto = TETRA_PROTO_LLC;
        ev.u.pdu.addr = mi->ssi;
        ev.u.pdu.encrypted = true;
        tmo_emit(rx, &ev);
        return;
    }
    tmo_llc(rx, mi, buf, s, e);
}

/* MAC-RESOURCE, table 21.55. Returns the end of the PDU or 0 to stop. */
static unsigned mac_resource(struct tetra_tmo_rx *rx, enum tmo_lchan ch,
                             const uint8_t *blk, unsigned start,
                             unsigned blk_end, int *flags)
{
    struct bitrd b;
    struct tmo_macinfo mi;
    unsigned fill, li, pdu_end, sdu_end;

    memset(&mi, 0, sizeof(mi));
    mi.tn = rx->t.tn;
    br_init(&b, blk, start + 2, blk_end);
    fill = br_get(&b, 1);
    br_skip(&b, 1); /* position of grant */
    mi.enc = (uint8_t)br_get(&b, 2);
    br_skip(&b, 1); /* random access flag */
    li = br_get(&b, 6);
    mi.addr_type = (uint8_t)br_get(&b, 3);

    if (ch == TMO_STCH && (li == LI_HALF2_STOLEN || li == LI_FRAG_START))
        *flags |= TMO_MAC_HALF2_STOLEN;
    if (mi.addr_type == TMO_ADDR_NULL || b.err)
        return 0;

    switch (mi.addr_type) {
    case TMO_ADDR_SSI:
    case TMO_ADDR_USSI:
    case TMO_ADDR_SMI:
        mi.ssi = br_get(&b, 24);
        break;
    case TMO_ADDR_EVENT:
        br_skip(&b, 10);
        break;
    case TMO_ADDR_SSI_EVENT:
    case TMO_ADDR_SMI_EVENT:
        mi.ssi = br_get(&b, 24);
        br_skip(&b, 10);
        break;
    case TMO_ADDR_SSI_UM:
        mi.ssi = br_get(&b, 24);
        mi.usage_marker = (uint8_t)br_get(&b, 6);
        break;
    }
    if (br_get(&b, 1))
        br_skip(&b, 4); /* power control */
    if (br_get(&b, 1))
        br_skip(&b, 8); /* basic slot granting */
    if (br_get(&b, 1)) {
        if (!parse_chan_alloc(&b, &mi.ca))
            return 0;
        mi.has_ca = true;
    }
    if (b.err)
        return 0;

    if (li == LI_FRAG_START || li == LI_HALF2_STOLEN) {
        pdu_end = blk_end;
    } else {
        pdu_end = li_bits(li);
        if (!pdu_end)
            return 0;
        pdu_end += start;
        if (pdu_end > blk_end)
            pdu_end = blk_end;
    }
    if (b.pos > pdu_end)
        return 0;

    sdu_end = fill ? bits_strip_fill(blk, b.pos, pdu_end) : pdu_end;
    if (li == LI_FRAG_START) {
        frag_start(rx, &mi, blk, b.pos, sdu_end);
        return 0;
    }
    if (b.pos == sdu_end) {
        /* No TM-SDU: still pass channel allocation / usage marker. */
        if (mi.has_ca || mi.usage_marker)
            tmo_llc(rx, &mi, blk, b.pos, b.pos);
    } else {
        deliver(rx, &mi, blk, b.pos, sdu_end);
    }
    return li == LI_HALF2_STOLEN ? 0 : pdu_end;
}

/* MAC-FRAG / MAC-END, tables 21.57 and 21.59. */
static unsigned mac_frag_end(struct tetra_tmo_rx *rx, const uint8_t *blk,
                             unsigned start, unsigned blk_end)
{
    struct bitrd b;
    struct tetra_tmo_frag *f = &rx->frag[rx->t.tn - 1];
    struct tmo_macinfo mi;
    unsigned sub, fill, li, pdu_end, sdu_end;

    br_init(&b, blk, start + 2, blk_end);
    sub = br_get(&b, 1);
    fill = br_get(&b, 1);

    if (!sub) {
        /* MAC-FRAG: the rest of the block */
        sdu_end = fill ? bits_strip_fill(blk, b.pos, blk_end) : blk_end;
        frag_append(rx, rx->t.tn, blk, b.pos, sdu_end);
        return 0;
    }

    memset(&mi, 0, sizeof(mi));
    br_skip(&b, 1); /* position of grant */
    li = br_get(&b, 6);
    if (br_get(&b, 1))
        br_skip(&b, 8);
    if (br_get(&b, 1)) {
        if (!parse_chan_alloc(&b, &mi.ca)) {
            f->active = false;
            return 0;
        }
        mi.has_ca = true;
    }
    pdu_end = li_bits(li);
    if (!pdu_end || b.err) {
        f->active = false;
        return 0;
    }
    pdu_end += start;
    if (pdu_end > blk_end)
        pdu_end = blk_end;
    if (b.pos > pdu_end) {
        f->active = false;
        return 0;
    }
    sdu_end = fill ? bits_strip_fill(blk, b.pos, pdu_end) : pdu_end;

    if (frag_append(rx, rx->t.tn, blk, b.pos, sdu_end)) {
        mi.tn = rx->t.tn;
        mi.addr_type = f->addr_type;
        mi.ssi = f->addr;
        mi.enc = f->enc;
        mi.usage_marker = f->usage_marker;
        f->active = false;
        deliver(rx, &mi, f->buf, 0, f->nbits);
    }
    return pdu_end;
}

/* MAC-D-BLCK, table 21.61: implicit length of a full slot. */
static void mac_d_blck(struct tetra_tmo_rx *rx, const uint8_t *blk,
                       unsigned start, unsigned blk_end)
{
    struct bitrd b;
    struct tmo_macinfo mi;
    unsigned fill, sdu_end;

    memset(&mi, 0, sizeof(mi));
    mi.tn = rx->t.tn;
    mi.addr_type = TMO_ADDR_EVENT;
    br_init(&b, blk, start + 2, blk_end);
    if (br_get(&b, 1))
        return; /* reserved subtype */
    fill = br_get(&b, 1);
    mi.enc = (uint8_t)br_get(&b, 2);
    br_skip(&b, 10 + 1);
    if (br_get(&b, 1))
        br_skip(&b, 8);
    if (b.err)
        return;
    sdu_end = fill ? bits_strip_fill(blk, b.pos, blk_end) : blk_end;
    deliver(rx, &mi, blk, b.pos, sdu_end);
}

int tmo_mac_block(struct tetra_tmo_rx *rx, enum tmo_lchan ch,
                  const uint8_t *blk, unsigned nbits)
{
    unsigned pos = 0;
    int flags = 0;

    /* PDU association: PDUs follow each other until a null PDU, a PDU
     * filling the block, or less than a null PDU of space is left. */
    while (pos + 16 <= nbits) {
        unsigned type = (bit_at(blk, pos) << 1) | bit_at(blk, pos + 1);
        unsigned next = 0;

        switch (type) {
        case 0:
            next = mac_resource(rx, ch, blk, pos, nbits, &flags);
            break;
        case 1:
            next = mac_frag_end(rx, blk, pos, nbits);
            break;
        case 2: {
            struct bitrd b;

            br_init(&b, blk, pos + 2, nbits);
            if (br_get(&b, 2) == 0)
                parse_sysinfo(rx, &b);
            break;
        }
        case 3:
            if (ch == TMO_STCH) {
                /* MAC-U-SIGNAL: U-plane data for the application */
                if (bit_at(blk, pos + 2))
                    flags |= TMO_MAC_HALF2_STOLEN;
            } else if (ch == TMO_SCH_F) {
                mac_d_blck(rx, blk, pos, nbits);
            }
            break;
        }
        if (!next || next <= pos)
            break;
        pos = next;
    }
    return flags;
}
