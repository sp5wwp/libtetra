/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <string.h>
#include <tetra/tetra_coding.h>
#include <tetra/tetra_tx.h>
#include "dmo_scenario.h"
#include "util/bits.h"

static uint32_t rnd(struct dmo_scn *s)
{
    s->rng ^= s->rng << 13;
    s->rng ^= s->rng >> 17;
    s->rng ^= s->rng << 5;
    return s->rng;
}

void scn_init(struct dmo_scn *s, scn_slot_cb cb, void *user, uint32_t seed)
{
    memset(s, 0, sizeof(*s));
    s->cb = cb;
    s->user = user;
    s->rng = seed ? seed : 1;
    s->tn = 1;
    s->fn = 1;
    s->mni = (262u << 14) | 1; /* MCC 262, MNC 1 */
    s->src = 2002;
    s->dst = 1001;
}

static void advance(struct dmo_scn *s)
{
    if (++s->tn > 4) {
        s->tn = 1;
        if (++s->fn > 18)
            s->fn = 1;
    }
}

void scn_idle(struct dmo_scn *s)
{
    uint8_t bits[510];

    for (unsigned i = 0; i < 510; i++)
        bits[i] = rnd(s) & 1;
    s->cb(s->user, bits, false);
    advance(s);
}

void scn_goto(struct dmo_scn *s, uint8_t fn, uint8_t tn)
{
    while (s->fn != fn || s->tn != tn)
        scn_idle(s);
}

static void emit_burst(struct dmo_scn *s, const uint8_t burst[470])
{
    uint8_t bits[510];

    for (unsigned i = 0; i < 510; i++)
        bits[i] = rnd(s) & 1;
    memcpy(bits + 34, burst, 470);
    s->cb(s->user, bits, true);
    advance(s);
}

/* Fill the rest of a MAC block: '1' then zeros. Returns true if used. */
static bool add_fill(uint8_t *buf, unsigned pos, unsigned cap)
{
    if (pos >= cap)
        return false;
    bw_put(buf, &pos, 1, 1);
    while (pos < cap)
        bw_put(buf, &pos, 0, 1);
    return true;
}

static void unpack(const uint8_t *packed, uint8_t *bits, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        bits[i] = bit_at(packed, i);
}

static unsigned build_sync(struct dmo_scn *s, uint8_t pdu[24],
                           const struct scn_msg *m, bool frag, uint8_t nfrag,
                           unsigned *l3_used)
{
    unsigned pos = 0, fillpos, avail, n;

    memset(pdu, 0, 24);
    /* SCH/S */
    bw_put(pdu, &pos, 0xd, 4); /* system code */
    bw_put(pdu, &pos, 0, 2);   /* DMAC-SYNC */
    bw_put(pdu, &pos, 0, 2);   /* direct MS-MS */
    bw_put(pdu, &pos, 0, 2);   /* reserved */
    bw_put(pdu, &pos, 0, 2);   /* A/B usage */
    bw_put(pdu, &pos, s->tn - 1u, 2);
    bw_put(pdu, &pos, s->fn, 5);
    bw_put(pdu, &pos, 0, 2); /* no encryption */
    bw_put(pdu, &pos, 0, 32);
    bw_put(pdu, &pos, 0, 7);
    /* SCH/H */
    bw_put(pdu, &pos, 0, 10);
    fillpos = pos;
    bw_put(pdu, &pos, 0, 1);
    bw_put(pdu, &pos, frag, 1);
    if (frag)
        bw_put(pdu, &pos, nfrag, 4);
    bw_put(pdu, &pos, m->frame_countdown, 2);
    bw_put(pdu, &pos, 0, 2);
    bw_put(pdu, &pos, s->dst, 24);
    bw_put(pdu, &pos, 0, 2);
    bw_put(pdu, &pos, s->src, 24);
    bw_put(pdu, &pos, s->mni, 24);
    bw_put(pdu, &pos, m->msg_type, 5);

    avail = 184 - pos;
    n = m->l3_bits < avail ? m->l3_bits : avail;
    bits_copy(pdu, pos, m->l3, 0, n);
    pos += n;
    if (add_fill(pdu, pos, 184)) {
        unsigned p = fillpos;

        bw_put(pdu, &p, 1, 1);
    }
    if (l3_used)
        *l3_used = n;
    return n;
}

static void send_dsb(struct dmo_scn *s, const uint8_t pdu[24])
{
    uint8_t t1[124], sb[120], bkn2[216], burst[470];
    uint32_t zero = tetra_scramb_init(0);

    unpack(pdu, t1, 60);
    tetra_encode_sch(TETRA_SCH_S, t1, zero, sb);
    for (unsigned i = 0; i < 124; i++)
        t1[i] = bit_at(pdu, 60 + i);
    tetra_encode_sch(TETRA_SCH_H, t1, zero, bkn2);
    tetra_dmo_build_dsb(burst, sb, bkn2);
    emit_burst(s, burst);
}

void scn_dsb(struct dmo_scn *s, const struct scn_msg *m)
{
    uint8_t pdu[24];

    build_sync(s, pdu, m, false, 0, NULL);
    send_dsb(s, pdu);
}

unsigned scn_dsb_frag(struct dmo_scn *s, const struct scn_msg *m, uint8_t nfrag)
{
    uint8_t pdu[24];
    unsigned used;

    build_sync(s, pdu, m, true, nfrag, &used);
    send_dsb(s, pdu);
    return used;
}

static uint32_t dcc(const struct dmo_scn *s)
{
    return tetra_scramb_init(tetra_dmo_colour_code(s->mni, s->src));
}

unsigned scn_schf_frag(struct dmo_scn *s, const struct scn_msg *m,
                       unsigned offset, bool end)
{
    uint8_t pdu[34], t1[268], t5[432], burst[470];
    unsigned pos = 0, n;

    memset(pdu, 0, sizeof(pdu));
    bw_put(pdu, &pos, 1, 2);
    bw_put(pdu, &pos, end, 1);
    bw_put(pdu, &pos, 0, 1); /* fill bit indication, patched below */
    n = m->l3_bits - offset;
    if (n > 264)
        n = 264;
    bits_copy(pdu, pos, m->l3, offset, n);
    pos += n;
    if (add_fill(pdu, pos, 268)) {
        unsigned p = 3;

        bw_put(pdu, &p, 1, 1);
    }
    unpack(pdu, t1, 268);
    tetra_encode_sch(TETRA_SCH_F, t1, dcc(s), t5);
    tetra_dmo_build_dnb(burst, t5, t5 + 216, false);
    emit_burst(s, burst);
    return n;
}

void scn_tch(struct dmo_scn *s, const uint8_t tch[432])
{
    uint8_t t5[432], burst[470];

    memcpy(t5, tch, 432);
    tetra_scramble(t5, 432, dcc(s));
    tetra_dmo_build_dnb(burst, t5, t5 + 216, false);
    emit_burst(s, burst);
}

/* DMAC-DATA in an STCH half slot (124 bits). */
static void build_data(struct dmo_scn *s, uint8_t pdu[16],
                       const struct scn_msg *m, bool half2)
{
    unsigned pos = 0, n;

    memset(pdu, 0, 16);
    bw_put(pdu, &pos, 0, 2);
    bw_put(pdu, &pos, 0, 1); /* fill, patched */
    bw_put(pdu, &pos, half2, 1);
    bw_put(pdu, &pos, 0, 1); /* no fragmentation */
    bw_put(pdu, &pos, 0, 1); /* not a null PDU */
    bw_put(pdu, &pos, m->frame_countdown, 2);
    bw_put(pdu, &pos, 0, 2);
    bw_put(pdu, &pos, 0, 2);
    bw_put(pdu, &pos, s->dst, 24);
    if (m->no_src) {
        bw_put(pdu, &pos, 2, 2);
    } else {
        bw_put(pdu, &pos, 0, 2);
        bw_put(pdu, &pos, s->src, 24);
    }
    bw_put(pdu, &pos, s->mni, 24);
    bw_put(pdu, &pos, m->msg_type, 5);
    n = m->l3_bits < 124 - pos ? m->l3_bits : 124 - pos;
    bits_copy(pdu, pos, m->l3, 0, n);
    pos += n;
    if (add_fill(pdu, pos, 124)) {
        unsigned p = 2;

        bw_put(pdu, &p, 1, 1);
    }
}

void scn_stch(struct dmo_scn *s, const struct scn_msg *m1,
              const struct scn_msg *m2, const uint8_t tch_half[216])
{
    uint8_t pdu[16], t1[124], b1[216], b2[216], burst[470];

    build_data(s, pdu, m1, m2 != NULL);
    unpack(pdu, t1, 124);
    tetra_encode_sch(TETRA_SCH_H, t1, dcc(s), b1);
    if (m2) {
        build_data(s, pdu, m2, false);
        unpack(pdu, t1, 124);
        tetra_encode_sch(TETRA_SCH_H, t1, dcc(s), b2);
    } else {
        memcpy(b2, tch_half, 216);
        tetra_scramble(b2, 216, dcc(s));
    }
    tetra_dmo_build_dnb(burst, b1, b2, true);
    emit_burst(s, burst);
}

void scn_tch_pattern(uint8_t tch[432], unsigned seed)
{
    uint32_t r = 0x9e3779b9u ^ (seed * 2654435761u);

    for (unsigned i = 0; i < 432; i++) {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        tch[i] = r & 1;
    }
}

/* Message dependent elements shared by DM-SETUP / DM-OCCUPIED. */
unsigned scn_l3_setup(uint8_t *buf, bool group, uint8_t prio)
{
    unsigned pos = 0;

    bw_put(buf, &pos, 0, 11); /* timing .. two-frequency flags */
    bw_put(buf, &pos, 0, 4);  /* speech */
    bw_put(buf, &pos, 0, 4);
    bw_put(buf, &pos, prio, 2);
    bw_put(buf, &pos, 0, 1); /* clear */
    bw_put(buf, &pos, group, 1);
    bw_put(buf, &pos, 0, 3);
    return pos;
}

unsigned scn_l3_tx_ceased(uint8_t *buf, uint8_t cause)
{
    unsigned pos = 0;

    bw_put(buf, &pos, 10, 6); /* reservation time remaining */
    bw_put(buf, &pos, 0, 1);  /* timing flag */
    bw_put(buf, &pos, 0, 1);  /* requests flag */
    bw_put(buf, &pos, 0, 1);  /* changeover requests flag */
    bw_put(buf, &pos, 0, 1);  /* recent user priority */
    bw_put(buf, &pos, 0, 1);  /* timing change announced */
    bw_put(buf, &pos, 0, 2);  /* priority */
    bw_put(buf, &pos, cause, 4);
    return pos;
}

unsigned scn_l3_reserved(uint8_t *buf, uint8_t remaining)
{
    unsigned pos = 0;

    bw_put(buf, &pos, 0, 1);
    bw_put(buf, &pos, remaining, 6);
    bw_put(buf, &pos, 0, 3); /* timing, requests, changeover flags */
    bw_put(buf, &pos, 0, 3); /* power class */
    bw_put(buf, &pos, 0, 1);
    bw_put(buf, &pos, 0, 2);
    return pos;
}

unsigned scn_l3_info(uint8_t *buf, uint32_t mni, uint32_t ssi)
{
    unsigned pos = 0;

    bw_put(buf, &pos, 0, 3); /* calling party TSI */
    bw_put(buf, &pos, mni, 24);
    bw_put(buf, &pos, ssi, 24);
    return pos;
}

unsigned scn_l3_release(uint8_t *buf, uint8_t cause)
{
    unsigned pos = 0;

    bw_put(buf, &pos, cause, 4);
    return pos;
}

static unsigned sds_mde(uint8_t *buf, unsigned pos, bool fcs)
{
    bw_put(buf, &pos, 0, 4); /* SDS time remaining */
    bw_put(buf, &pos, 0, 1); /* stand-alone */
    bw_put(buf, &pos, 0, 2); /* priority */
    bw_put(buf, &pos, fcs, 1);
    return pos;
}

unsigned scn_l3_sds_status(uint8_t *buf, uint16_t status)
{
    unsigned pos = sds_mde(buf, 0, false);

    bw_put(buf, &pos, 0, 1); /* no additional addressing */
    bw_put(buf, &pos, 4, 4); /* pre-coded status */
    bw_put(buf, &pos, status, 16);
    return pos;
}

static void send_repeated_dsb(struct dmo_scn *s, struct scn_msg *m,
                              unsigned frames)
{
    for (unsigned f = 0; f < frames; f++) {
        m->frame_countdown = (uint8_t)(frames - 1 - f);
        while (s->tn != 1)
            scn_idle(s);
        scn_dsb(s, m);
        scn_idle(s);
        scn_dsb(s, m);
        scn_idle(s);
    }
}

unsigned scn_run_reference(struct dmo_scn *s)
{
    uint8_t l3[512], tch[432];
    struct scn_msg m;
    unsigned ntraffic = 0;

    /* 1. idle channel, two frames */
    for (unsigned i = 0; i < 8; i++)
        scn_idle(s);

    /* 2. call set-up in slots 1 and 3 of two frames */
    memset(&m, 0, sizeof(m));
    m.msg_type = 8; /* DM-SETUP */
    m.l3 = l3;
    m.l3_bits = scn_l3_setup(l3, true, 1);
    send_repeated_dsb(s, &m, 2);

    /* traffic for 30 frames */
    for (unsigned k = 0; k < 30; k++) {
        if (s->fn == 18) {
            m.msg_type = 13; /* DM-OCCUPIED in slots 1 and 3 */
            m.frame_countdown = 0;
            m.l3_bits = scn_l3_setup(l3, true, 1);
            scn_dsb(s, &m);
            scn_idle(s);
            scn_dsb(s, &m);
            scn_idle(s);
            continue;
        }
        if (k == 5) {
            /* stolen first half: TPNI without source address */
            struct scn_msg info = { 21, 0, true, l3, 0 };

            info.l3_bits = scn_l3_info(l3, s->mni, SCN_TPNI_SSI);
            scn_tch_pattern(tch, s->fn);
            scn_stch(s, &info, NULL, tch + 216);
        } else {
            scn_tch_pattern(tch, s->fn);
            scn_tch(s, tch);
        }
        ntraffic++;
        scn_idle(s);
        if (s->fn == 6 || s->fn == 12) {
            m.msg_type = 13;
            m.frame_countdown = 0;
            m.l3_bits = scn_l3_setup(l3, true, 1);
            scn_dsb(s, &m);
        } else {
            scn_idle(s);
        }
        scn_idle(s);
    }
    if (s->fn == 18) {
        for (unsigned i = 0; i < 4; i++)
            scn_idle(s);
    }

    /* DM-TX CEASED on STCH + STCH in two traffic slots */
    for (unsigned r = 0; r < 2; r++) {
        struct scn_msg c = { 15, (uint8_t)(1 - r), false, l3, 0 };

        c.l3_bits = scn_l3_tx_ceased(l3, 1);
        scn_stch(s, &c, &c, NULL);
        scn_idle(s);
        scn_idle(s);
        scn_idle(s);
    }

    /* 3. reservation: DM-RESERVED in frames 6, 12, 18 down to 0 */
    uint8_t target = s->fn < 6 ? 6 : (s->fn < 12 ? 12 : 18);

    for (int rem = 2; rem >= 0; rem--) {
        scn_goto(s, target, 1);
        target = target == 18 ? 6 : (uint8_t)(target + 6);
        m.msg_type = 0;
        m.frame_countdown = 0;
        m.l3_bits = scn_l3_reserved(l3, (uint8_t)rem);
        scn_dsb(s, &m);
        scn_idle(s);
    }
    for (unsigned i = 0; i < 12; i++)
        scn_idle(s);

    /* 4a. status SDS in DSBs, slots 1 and 3, three frames */
    s->src = 3003;
    s->dst = 2002;
    memset(&m, 0, sizeof(m));
    m.msg_type = 22; /* DM-SDS UDATA */
    m.l3 = l3;
    m.l3_bits = scn_l3_sds_status(l3, SCN_STATUS);
    send_repeated_dsb(s, &m, 3);
    for (unsigned i = 0; i < 12; i++)
        scn_idle(s);

    /* 4b. fragmented text: DMAC-SYNC x2 frames, then SCH/F fragments */
    m.l3_bits = scn_l3_sds_text(l3, SCN_TEXT, true);
    {
        unsigned nfrag, used = 0, off;

        /* bits left after the DMAC-SYNC: 264 per fragment */
        unsigned head_cap =
            184 - (60 + 10 + 1 + 1 + 4 + 2 + 2 + 24 + 2 + 24 + 24 + 5);
        unsigned rest = m.l3_bits > head_cap ? m.l3_bits - head_cap : 0;

        nfrag = (uint8_t)((rest + 263) / 264);
        for (unsigned f = 0; f < 2; f++) {
            m.frame_countdown = (uint8_t)(1 - f);
            while (s->tn != 1)
                scn_idle(s);
            used = scn_dsb_frag(s, &m, (uint8_t)nfrag);
            scn_idle(s);
            scn_dsb_frag(s, &m, (uint8_t)nfrag);
            scn_idle(s);
        }
        off = used;
        for (unsigned f = 0; f < nfrag; f++) {
            if (s->fn == 18) {
                for (unsigned i = 0; i < 4; i++)
                    scn_idle(s);
            }
            off += scn_schf_frag(s, &m, off, f == nfrag - 1);
            scn_idle(s);
            scn_idle(s);
            scn_idle(s);
        }
    }
    for (unsigned i = 0; i < 16; i++)
        scn_idle(s);
    return ntraffic;
}

unsigned scn_l3_sds_text(uint8_t *buf, const char *latin1, bool fcs)
{
    unsigned pos = sds_mde(buf, 0, fcs), sdu, len = strlen(latin1);
    unsigned li = 8 + 4 + 2 + 1 + 1 + 8 + 1 + 7 + 8 * len;

    sdu = pos;
    bw_put(buf, &pos, 0, 1);
    bw_put(buf, &pos, 3, 4); /* SDTI: length indicator + UDD4 */
    bw_put(buf, &pos, li, 11);
    bw_put(buf, &pos, 0x82, 8); /* text messaging, SDS-TL */
    bw_put(buf, &pos, 0, 4);    /* SDS-TRANSFER */
    bw_put(buf, &pos, 0, 2);    /* no report */
    bw_put(buf, &pos, 0, 1);
    bw_put(buf, &pos, 0, 1);  /* no storage */
    bw_put(buf, &pos, 42, 8); /* message reference */
    bw_put(buf, &pos, 0, 1);  /* no time stamp */
    bw_put(buf, &pos, 1, 7);  /* ISO 8859-1 */
    for (unsigned i = 0; i < len; i++)
        bw_put(buf, &pos, (uint8_t)latin1[i], 8);
    if (fcs) {
        uint32_t f = tetra_fcs32_packed(buf, sdu, pos - sdu);

        bw_put(buf, &pos, f, 32);
    }
    return pos;
}
