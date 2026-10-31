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
        c->have_hyperframe = false;
        c->have_cck_id = false;
        c->hyperframe = 0;
        c->cck_id = 0;
        for (unsigned i = 0; i < 4; i++) {
            rx->frag[i].active = false;
#if TETRA_ENABLE_CRYPTO
            memset(rx->frag[i].eck, 0, sizeof(rx->frag[i].eck));
#endif
        }
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
    n.have_hyperframe = rx->cell.have_hyperframe;
    n.have_cck_id = rx->cell.have_cck_id;
    if (memcmp(&n, &rx->cell, sizeof(n))) {
        rx->cell = n;
        if (is_cck) {
            rx->cell.cck_id = hf;
            rx->cell.have_cck_id = true;
        } else {
            rx->cell.hyperframe = hf;
            rx->cell.have_hyperframe = true;
        }
        emit_cell(rx);
        return;
    }
    if (is_cck) {
        rx->cell.cck_id = hf;
        rx->cell.have_cck_id = true;
    } else {
        rx->cell.hyperframe = hf;
        rx->cell.have_hyperframe = true;
    }
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

#if TETRA_ENABLE_CRYPTO
static void key_request_from_mac(const struct tmo_macinfo *mi,
                                 struct tetra_tmo_key_request *request)
{
    request->address = mi->ssi;
    request->usage_marker = mi->usage_marker;
    request->traffic = false;
    request->encryption_mode = mi->encryption_mode;
    request->encryption_mode_valid = true;
}
#endif /* TETRA_ENABLE_CRYPTO */

static void frag_start(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
                       const uint8_t *blk, unsigned s, unsigned e,
                       unsigned cipher_offset_bits)
{
    struct tetra_tmo_frag *f = &rx->frag[mi->tn - 1];

    memset(f, 0, sizeof(*f));
    f->active = true;
    f->encrypted = tmo_macinfo_encrypted(mi);
    f->encryption_mode = mi->encryption_mode;
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

#if TETRA_ENABLE_CRYPTO
    f->crypto_valid = !f->encrypted;
    if (f->encrypted) {
        struct tetra_tmo_key_request request;

        key_request_from_mac(mi, &request);
        request.cell = &rx->cell;
        if (tmo_crypto_get_eck(rx, &request, &f->algorithm, f->eck) &&
            tmo_crypto_xor_bits(rx, f->algorithm, f->eck, f->buf, 0,
                                f->nbits, cipher_offset_bits))
            f->crypto_valid = true;
        else
            memset(f->eck, 0, sizeof(f->eck));
    }
#else
    (void)rx;
    (void)cipher_offset_bits;
#endif
}

static bool frag_append(struct tetra_tmo_rx *rx, uint8_t tn,
                        const uint8_t *blk, unsigned s, unsigned e,
                        unsigned cipher_offset_bits)
{
    struct tetra_tmo_frag *f = &rx->frag[tn - 1];
    unsigned old_nbits;

    if (!f->active)
        return false;
    if (f->nbits + (e - s) > TETRA_TMO_MAX_SDU_BITS) {
        f->active = false;
#if TETRA_ENABLE_CRYPTO
        memset(f->eck, 0, sizeof(f->eck));
#endif
        return false;
    }
    old_nbits = f->nbits;
    bits_copy(f->buf, old_nbits, blk, s, e - s);
    f->nbits = (uint16_t)(f->nbits + (e - s));
#if TETRA_ENABLE_CRYPTO
    if (f->encrypted && f->crypto_valid &&
        !tmo_crypto_xor_bits(rx, f->algorithm, f->eck, f->buf, old_nbits,
                             e - s, cipher_offset_bits)) {
        f->active = false;
        memset(f->eck, 0, sizeof(f->eck));
        return false;
    }
#else
    (void)rx;
    (void)cipher_offset_bits;
#endif
    return true;
}

static void emit_encrypted_llc(struct tetra_tmo_rx *rx,
                               const struct tmo_macinfo *mi)
{
    struct tetra_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_PDU;
    ev.u.pdu.proto = TETRA_PROTO_LLC;
    ev.u.pdu.addr = mi->ssi;
    ev.u.pdu.encrypted = true;
    tmo_emit(rx, &ev);
}

#if TETRA_ENABLE_CRYPTO
static void frag_start_plain(struct tetra_tmo_rx *rx,
                             const struct tmo_macinfo *mi,
                             const uint8_t *buf, unsigned s, unsigned e,
                             enum tetra_tea_algorithm algorithm,
                             const uint8_t eck[TETRA_TEA_KEY_BYTES])
{
    struct tetra_tmo_frag *f = &rx->frag[mi->tn - 1];

    memset(f, 0, sizeof(*f));
    if (e - s > TETRA_TMO_MAX_SDU_BITS)
        return;
    f->active = true;
    f->encrypted = tmo_macinfo_encrypted(mi);
    f->encryption_mode = mi->encryption_mode;
    f->addr_type = mi->addr_type;
    f->addr = mi->ssi;
    f->usage_marker = mi->usage_marker;
    f->crypto_valid = true;
    f->algorithm = algorithm;
    memcpy(f->eck, eck, sizeof(f->eck));
    f->nbits = (uint16_t)(e - s);
    bits_copy(f->buf, 0, buf, s, e - s);
}

static bool frag_append_plain(struct tetra_tmo_rx *rx, uint8_t tn,
                              const uint8_t *buf, unsigned s, unsigned e)
{
    struct tetra_tmo_frag *f = &rx->frag[tn - 1];

    if (!f->active)
        return false;
    if (f->nbits + (e - s) > TETRA_TMO_MAX_SDU_BITS) {
        f->active = false;
#if TETRA_ENABLE_CRYPTO
        memset(f->eck, 0, sizeof(f->eck));
#endif
        return false;
    }
    bits_copy(f->buf, f->nbits, buf, s, e - s);
    f->nbits = (uint16_t)(f->nbits + (e - s));
    return true;
}
#endif /* TETRA_ENABLE_CRYPTO */

static void deliver(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
                    const uint8_t *buf, unsigned s, unsigned e,
                    unsigned cipher_offset_bits)
{
#if !TETRA_ENABLE_CRYPTO
    (void)cipher_offset_bits;
#endif
    if (e <= s)
        return;
    if (mi->encryption_mode == TETRA_TMO_ENC_RESERVED)
        return;
    if (tmo_macinfo_encrypted(mi)) {
#if TETRA_ENABLE_CRYPTO
        struct tetra_tmo_key_request request;
        enum tetra_tea_algorithm algorithm;
        uint8_t eck[TETRA_TEA_KEY_BYTES];
        uint8_t tmp[(TETRA_TMO_MAX_SDU_BITS + 7) / 8] = { 0 };
        unsigned nbits = e - s;

        if (nbits > TETRA_TMO_MAX_SDU_BITS) {
            emit_encrypted_llc(rx, mi);
            return;
        }
        bits_copy(tmp, 0, buf, s, nbits);
        key_request_from_mac(mi, &request);
        request.cell = &rx->cell;
        if (tmo_crypto_get_eck(rx, &request, &algorithm, eck) &&
            tmo_crypto_xor_bits(rx, algorithm, eck, tmp, 0, nbits,
                                cipher_offset_bits)) {
            memset(eck, 0, sizeof(eck));
            /* Keep the original ENC field so call state retains the active
             * key-identifier parity. The copied payload is already clear. */
            tmo_llc(rx, mi, tmp, 0, nbits);
            return;
        }
        memset(eck, 0, sizeof(eck));
        emit_encrypted_llc(rx, mi);
        return;
#else
        emit_encrypted_llc(rx, mi);
        return;
#endif
    }
    tmo_llc(rx, mi, buf, s, e);
}

/* MAC-RESOURCE, table 21.55. Returns the end of the PDU or 0 to stop. */
static unsigned mac_resource(struct tetra_tmo_rx *rx, enum tmo_lchan ch,
                             const uint8_t *blk, unsigned start,
                             unsigned blk_end, int *flags,
                             unsigned cipher_offset_bits)
{
    struct bitrd b;
    struct tmo_macinfo mi;
    unsigned fill, li, pdu_end, sdu_end;
    bool ca_present;
#if TETRA_ENABLE_CRYPTO
    unsigned ca_start = 0;
#endif

    memset(&mi, 0, sizeof(mi));
    mi.tn = rx->t.tn;
    br_init(&b, blk, start + 2, blk_end);
    fill = br_get(&b, 1);
    br_skip(&b, 1); /* position of grant */
    mi.encryption_mode = (uint8_t)br_get(&b, 2);
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
    ca_present = br_get(&b, 1);
    if (ca_present) {
        mi.has_ca = true;
#if TETRA_ENABLE_CRYPTO
        ca_start = b.pos;
#endif
        /* The channel-allocation element is itself ciphered when ENC != 0. */
        if (mi.encryption_mode == TETRA_TMO_ENC_CLEAR &&
            !parse_chan_alloc(&b, &mi.ca))
            return 0;
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

    if (mi.encryption_mode == TETRA_TMO_ENC_RESERVED)
        return pdu_end;

#if TETRA_ENABLE_CRYPTO
    if (tmo_macinfo_encrypted(&mi) && ca_present) {
        struct tetra_tmo_key_request request;
        enum tetra_tea_algorithm algorithm;
        uint8_t eck[TETRA_TEA_KEY_BYTES];
        uint8_t tmp[(TETRA_TMO_MAX_SDU_BITS + 7) / 8] = { 0 };
        unsigned cipher_start = ca_start;
        unsigned nbits = sdu_end - cipher_start;
        struct bitrd ca;
        bool decrypted = false;

        if (nbits <= TETRA_TMO_MAX_SDU_BITS) {
            bits_copy(tmp, 0, blk, cipher_start, nbits);
            key_request_from_mac(&mi, &request);
            request.cell = &rx->cell;
            decrypted = tmo_crypto_get_eck(rx, &request, &algorithm, eck) &&
                        tmo_crypto_xor_bits(rx, algorithm, eck, tmp, 0,
                                            nbits, cipher_offset_bits);
        }
        if (!decrypted) {
            memset(eck, 0, sizeof(eck));
            if (li == LI_FRAG_START)
                rx->frag[mi.tn - 1].active = false;
            emit_encrypted_llc(rx, &mi);
            return li == LI_HALF2_STOLEN ? 0 : pdu_end;
        }

        br_init(&ca, tmp, 0, nbits);
        if (!parse_chan_alloc(&ca, &mi.ca) || ca.err) {
            memset(eck, 0, sizeof(eck));
            return 0;
        }
        mi.has_ca = true;
        if (li == LI_FRAG_START) {
            frag_start_plain(rx, &mi, tmp, ca.pos, nbits, algorithm, eck);
            memset(eck, 0, sizeof(eck));
            return 0;
        }
        if (ca.pos == nbits) {
            if (mi.has_ca || mi.usage_marker)
                tmo_llc(rx, &mi, tmp, nbits, nbits);
        } else {
            /* Keep ENC metadata while passing the already-deciphered SDU. */
            tmo_llc(rx, &mi, tmp, ca.pos, nbits);
        }
        memset(eck, 0, sizeof(eck));
        return li == LI_HALF2_STOLEN ? 0 : pdu_end;
    }
#endif /* TETRA_ENABLE_CRYPTO */

    if (li == LI_FRAG_START) {
        frag_start(rx, &mi, blk, b.pos, sdu_end, cipher_offset_bits);
        return 0;
    }
    if (b.pos == sdu_end) {
        /* No TM-SDU: still pass channel allocation / usage marker. */
        if (mi.has_ca || mi.usage_marker)
            tmo_llc(rx, &mi, blk, b.pos, b.pos);
    } else {
        deliver(rx, &mi, blk, b.pos, sdu_end, cipher_offset_bits);
    }
    return li == LI_HALF2_STOLEN ? 0 : pdu_end;
}

/* MAC-FRAG / MAC-END, tables 21.57 and 21.59. */
static unsigned mac_frag_end(struct tetra_tmo_rx *rx, const uint8_t *blk,
                             unsigned start, unsigned blk_end,
                             unsigned cipher_offset_bits)
{
    struct bitrd b;
    struct tetra_tmo_frag *f = &rx->frag[rx->t.tn - 1];
    struct tmo_macinfo mi;
    unsigned sub, fill, li, pdu_end, sdu_end;
    bool ca_present;
#if TETRA_ENABLE_CRYPTO
    unsigned ca_start = 0;
#endif

    br_init(&b, blk, start + 2, blk_end);
    sub = br_get(&b, 1);
    fill = br_get(&b, 1);

    if (!sub) {
        /* MAC-FRAG: the rest of the block; ciphering starts at this segment. */
        sdu_end = fill ? bits_strip_fill(blk, b.pos, blk_end) : blk_end;
        frag_append(rx, rx->t.tn, blk, b.pos, sdu_end, cipher_offset_bits);
        return 0;
    }

    memset(&mi, 0, sizeof(mi));
    br_skip(&b, 1); /* position of grant */
    li = br_get(&b, 6);
    if (br_get(&b, 1))
        br_skip(&b, 8);
    ca_present = br_get(&b, 1);
    if (ca_present) {
#if TETRA_ENABLE_CRYPTO
        ca_start = b.pos;
#endif
        mi.has_ca = true;
        /* For an encrypted fragment, CA is decrypted before it is parsed. */
        if (!f->encrypted && !parse_chan_alloc(&b, &mi.ca)) {
            f->active = false;
            return 0;
        }
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

#if TETRA_ENABLE_CRYPTO
    if (ca_present && f->encrypted) {
        uint8_t tmp[(TETRA_TMO_MAX_SDU_BITS + 7) / 8] = { 0 };
        unsigned nbits = sdu_end - ca_start;
        struct bitrd ca;

        if (!f->active || !f->crypto_valid ||
            nbits > TETRA_TMO_MAX_SDU_BITS) {
            mi.tn = rx->t.tn;
            mi.addr_type = f->addr_type;
            mi.ssi = f->addr;
            emit_encrypted_llc(rx, &mi);
            f->active = false;
            memset(f->eck, 0, sizeof(f->eck));
            return pdu_end;
        }
        bits_copy(tmp, 0, blk, ca_start, nbits);
        if (!tmo_crypto_xor_bits(rx, f->algorithm, f->eck, tmp, 0, nbits,
                                 cipher_offset_bits)) {
            f->active = false;
            memset(f->eck, 0, sizeof(f->eck));
            return pdu_end;
        }
        br_init(&ca, tmp, 0, nbits);
        if (!parse_chan_alloc(&ca, &mi.ca) || ca.err ||
            !frag_append_plain(rx, rx->t.tn, tmp, ca.pos, nbits)) {
            f->active = false;
            memset(f->eck, 0, sizeof(f->eck));
            return pdu_end;
        }
    } else
#endif
    {
        if (!frag_append(rx, rx->t.tn, blk, b.pos, sdu_end,
                         cipher_offset_bits))
            return pdu_end;
    }

    if (f->active) {
        mi.tn = rx->t.tn;
        mi.addr_type = f->addr_type;
        mi.ssi = f->addr;
        mi.encryption_mode = f->encryption_mode;
        mi.usage_marker = f->usage_marker;
        f->active = false;
#if TETRA_ENABLE_CRYPTO
        if (f->encrypted && !f->crypto_valid)
            emit_encrypted_llc(rx, &mi);
        else
            /* The fragment is already decrypted (if encryption was active). */
            tmo_llc(rx, &mi, f->buf, 0, f->nbits);
#else
        if (f->encrypted)
            emit_encrypted_llc(rx, &mi);
        else
            tmo_llc(rx, &mi, f->buf, 0, f->nbits);
#endif
#if TETRA_ENABLE_CRYPTO
        memset(f->eck, 0, sizeof(f->eck));
#endif
    }
    return pdu_end;
}

/* MAC-D-BLCK, table 21.61: implicit length of a full slot. */
static void mac_d_blck(struct tetra_tmo_rx *rx, const uint8_t *blk,
                       unsigned start, unsigned blk_end,
                       unsigned cipher_offset_bits)
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
    mi.encryption_mode = (uint8_t)br_get(&b, 2);
    br_skip(&b, 10 + 1);
    if (br_get(&b, 1))
        br_skip(&b, 8);
    if (b.err)
        return;
    sdu_end = fill ? bits_strip_fill(blk, b.pos, blk_end) : blk_end;
    deliver(rx, &mi, blk, b.pos, sdu_end, cipher_offset_bits);
}

int tmo_mac_block(struct tetra_tmo_rx *rx, enum tmo_lchan ch,
                  const uint8_t *blk, unsigned nbits,
                  unsigned cipher_offset_bits)
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
            next = mac_resource(rx, ch, blk, pos, nbits, &flags,
                                cipher_offset_bits);
            break;
        case 1:
            next = mac_frag_end(rx, blk, pos, nbits, cipher_offset_bits);
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
                mac_d_blck(rx, blk, pos, nbits, cipher_offset_bits);
            }
            break;
        }
        if (!next || next <= pos)
            break;
        pos = next;
    }
    return flags;
}
