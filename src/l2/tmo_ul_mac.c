/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * TMO upper MAC, uplink: EN 300 392-2 clause 21.4.2.
 * MAC-ACCESS and MAC-END-HU on SCH/HU; MAC-DATA, MAC-FRAG, MAC-END and
 * MAC-U-SIGNAL on SCH/F and STCH. Only π/4-DQPSK (Y2 = Z2 = 1 octet).
 */

#include <string.h>
#include "tmo_int.h"

#if TETRA_UPLINK_DECODE

#define UL_LI_HALF2_STOLEN 62
#define UL_LI_FRAG_START 63

/*
 * Uplink fragments are reassembled in the downlink buffer of TN1: the
 * uplink is decoded only while not synchronised to a downlink, and the
 * fragment buffers are cleared whenever synchronisation changes.
 */
static struct tetra_tmo_frag *ul_frag(struct tetra_tmo_rx *rx)
{
    return &rx->frag[0];
}

/* Address type and address: SSI, event label, USSI or SMI. */
static uint32_t ul_addr(struct bitrd *b)
{
    if (br_get(b, 2) == 1) {
        br_skip(b, 10); /* event label: the MS is not identified */
        return 0;
    }
    return br_get(b, 24);
}

static void ul_frag_start(struct tetra_tmo_rx *rx, uint32_t ssi,
                          const uint8_t *blk, unsigned s, unsigned e)
{
    struct tetra_tmo_frag *f = ul_frag(rx);

    f->active = false;
    if (e - s > TETRA_TMO_MAX_SDU_BITS)
        return;
    f->active = true;
    f->encrypted = false;
    f->addr = ssi;
    bits_copy(f->buf, 0, blk, s, e - s);
    f->nbits = (uint16_t)(e - s);
}

static bool ul_frag_add(struct tetra_tmo_rx *rx, const uint8_t *blk, unsigned s,
                        unsigned e)
{
    struct tetra_tmo_frag *f = ul_frag(rx);

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

static void ul_frag_end(struct tetra_tmo_rx *rx, const uint8_t *blk, unsigned s,
                        unsigned e)
{
    struct tetra_tmo_frag *f = ul_frag(rx);

    if (!ul_frag_add(rx, blk, s, e))
        return;
    f->active = false;
    tmo_ul_llc(rx, f->addr, f->buf, 0, f->nbits);
}

static void ul_sdu(struct tetra_tmo_rx *rx, uint32_t ssi, bool encrypted,
                   bool frag, const uint8_t *blk, unsigned s, unsigned e)
{
    if (encrypted) {
        ul_frag(rx)->active = false;
        tmo_ul_encrypted(rx, ssi);
    } else if (frag) {
        ul_frag_start(rx, ssi, blk, s, e);
    } else {
        tmo_ul_llc(rx, ssi, blk, s, e);
    }
}

/* MAC-ACCESS on SCH/HU. */
static void mac_access(struct tetra_tmo_rx *rx, const uint8_t *blk,
                       unsigned nbits)
{
    struct bitrd b;
    unsigned fill, end = nbits, sdu_end;
    bool enc, frag = false;
    uint32_t ssi;

    br_init(&b, blk, 1, nbits);
    fill = br_get(&b, 1);
    enc = br_get(&b, 1);
    ssi = ul_addr(&b);
    if (br_get(&b, 1)) { /* optional field */
        if (!br_get(&b, 1)) {
            unsigned li = br_get(&b, 5);

            if (li < 3 || li > 12)
                return; /* null PDU or reserved */
            if (li * 8 < end)
                end = li * 8;
        } else {
            frag = br_get(&b, 1); /* capacity request */
            br_skip(&b, 4);       /* reservation requirement */
        }
    }
    if (b.err || b.pos > end)
        return;
    sdu_end = fill ? bits_strip_fill(blk, b.pos, end) : end;
    ul_sdu(rx, ssi, enc, frag, blk, b.pos, sdu_end);
}

/* MAC-END-HU on SCH/HU. */
static void mac_end_hu(struct tetra_tmo_rx *rx, const uint8_t *blk,
                       unsigned nbits)
{
    struct bitrd b;
    unsigned fill, end = nbits;

    br_init(&b, blk, 1, nbits);
    fill = br_get(&b, 1);
    if (!br_get(&b, 1)) {
        unsigned li = br_get(&b, 4);

        if (li == 0 || li > 12) {
            ul_frag(rx)->active = false;
            return;
        }
        if (li * 8 < end)
            end = li * 8;
    } else {
        br_skip(&b, 4); /* reservation requirement */
    }
    if (b.err || b.pos > end)
        return;
    ul_frag_end(rx, blk, b.pos, fill ? bits_strip_fill(blk, b.pos, end) : end);
}

/* MAC-DATA. Returns the end of the PDU or 0 to stop. */
static unsigned mac_data(struct tetra_tmo_rx *rx, enum tmo_ul_lchan ch,
                         const uint8_t *blk, unsigned start, unsigned nbits,
                         int *flags)
{
    struct bitrd b;
    unsigned fill, pdu_end = nbits, sdu_end;
    bool enc, frag = false;
    uint32_t ssi;

    br_init(&b, blk, start + 2, nbits);
    fill = br_get(&b, 1);
    enc = br_get(&b, 1);
    ssi = ul_addr(&b);
    if (!br_get(&b, 1)) {
        unsigned li = br_get(&b, 6);

        if (li == UL_LI_HALF2_STOLEN) {
            if (ch == TMO_UL_STCH)
                *flags |= TMO_MAC_HALF2_STOLEN;
        } else if (li == UL_LI_FRAG_START) {
            frag = true;
        } else if (li >= 3 && li <= 34) {
            if (start + li * 8 < nbits)
                pdu_end = start + li * 8;
        } else {
            return 0; /* null PDU or reserved */
        }
    } else {
        frag = br_get(&b, 1); /* capacity request */
        br_skip(&b, 4 + 1);   /* reservation requirement, reserved */
    }
    if (frag && ch == TMO_UL_STCH)
        *flags |= TMO_MAC_HALF2_STOLEN; /* continues in block 2 */
    if (b.err || b.pos > pdu_end)
        return 0;
    sdu_end = fill ? bits_strip_fill(blk, b.pos, pdu_end) : pdu_end;
    ul_sdu(rx, ssi, enc, frag, blk, b.pos, sdu_end);
    return frag || pdu_end >= nbits ? 0 : pdu_end;
}

/* MAC-FRAG and MAC-END, uplink. */
static unsigned mac_frag_end(struct tetra_tmo_rx *rx, const uint8_t *blk,
                             unsigned start, unsigned nbits)
{
    struct bitrd b;
    unsigned sub, fill, lirr, pdu_end = nbits;

    br_init(&b, blk, start + 2, nbits);
    sub = br_get(&b, 1);
    fill = br_get(&b, 1);
    if (!sub) {
        ul_frag_add(rx, blk, b.pos,
                    fill ? bits_strip_fill(blk, b.pos, nbits) : nbits);
        return 0;
    }
    /* Length indication (2..34 octets) or reservation requirement */
    lirr = br_get(&b, 6);
    if (lirr >= 2 && lirr <= 34) {
        if (start + lirr * 8 < nbits)
            pdu_end = start + lirr * 8;
    } else if (lirr < 48) {
        ul_frag(rx)->active = false;
        return 0;
    }
    if (b.err || b.pos > pdu_end)
        return 0;
    ul_frag_end(rx, blk, b.pos,
                fill ? bits_strip_fill(blk, b.pos, pdu_end) : pdu_end);
    return pdu_end >= nbits ? 0 : pdu_end;
}

int tmo_ul_block(struct tetra_tmo_rx *rx, enum tmo_ul_lchan ch,
                 const uint8_t *blk, unsigned nbits)
{
    unsigned pos = 0;
    int flags = 0;

    if (ch == TMO_UL_SCH_HU) {
        if (bit_at(blk, 0))
            mac_end_hu(rx, blk, nbits);
        else
            mac_access(rx, blk, nbits);
        return 0;
    }

    /* PDU association as on the downlink */
    while (pos + 16 <= nbits) {
        unsigned type = (bit_at(blk, pos) << 1) | bit_at(blk, pos + 1);
        unsigned next = 0;

        switch (type) {
        case 0:
            next = mac_data(rx, ch, blk, pos, nbits, &flags);
            break;
        case 1:
            next = mac_frag_end(rx, blk, pos, nbits);
            break;
        case 3:
            /* MAC-U-SIGNAL (STCH): U-plane signalling for the
             * application. MAC-U-BLCK (SCH/F) is addressed by an event
             * label assigned on the downlink and is not decoded. */
            if (ch == TMO_UL_STCH && bit_at(blk, pos + 2))
                flags |= TMO_MAC_HALF2_STOLEN;
            break;
        default:
            break;
        }
        if (!next || next <= pos)
            break;
        pos = next;
    }
    return flags;
}

#endif /* TETRA_UPLINK_DECODE */
