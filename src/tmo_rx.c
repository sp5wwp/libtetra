/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * TMO receiver: downlink synchronisation, TDMA tracking and slot dispatch;
 * uplink burst recognition and decoding while not synchronised.
 * EN 300 392-2 clauses 9.4.4, 9.5 and 19.
 */

#include <string.h>
#include <tetra/tetra_coding.h>
#include <tetra/tetra_tmo.h>
#include "l1/l1.h"
#include "l2/lmac.h"
#include "tmo_int.h"

/* Correlation (per mille) needed to accept a training sequence. */
#define TRAIN_MIN 600
/* Bits searched either side of the expected burst position. */
#define TIMING_SEARCH 4
/* Correlation (per mille) accepted without trying other offsets. */
#define TRAIN_SURE 900

/* Burst offsets tried, nearest first. */
static const int8_t search_order[] = { 0, -2, 2, -4, 4 };
/* Slots without a recognisable burst before declaring loss of lock. */
#define LOCK_LOSS_SLOTS 144

void tmo_emit(struct tetra_tmo_rx *rx, struct tetra_event *ev)
{
    ev->mode = TETRA_MODE_TMO;
    ev->uplink = rx->in_uplink;
    if (rx->in_uplink)
        memset(&ev->time, 0, sizeof(ev->time));
    else
        ev->time = rx->t;
    if (rx->cfg.cb)
        rx->cfg.cb(rx->cfg.user, ev);
}

bool tmo_addressed(const struct tetra_tmo_rx *rx, uint32_t ssi)
{
    if (!ssi)
        return false;
    if (rx->cfg.issi && ssi == rx->cfg.issi)
        return true;
    for (unsigned i = 0; i < rx->cfg.ngroups; i++) {
        if (rx->cfg.groups[i] == ssi)
            return true;
    }
    return false;
}

void tetra_tmo_init(struct tetra_tmo_rx *rx, const struct tetra_rx_config *cfg)
{
    memset(rx, 0, sizeof(*rx));
    rx->cfg = *cfg;
    rx->slots = TETRA_TS_ALL;
    rx->follow.tuned = 0xffff;
    memset(rx->dl_um, 0xff, sizeof(rx->dl_um));
}

void tetra_tmo_set_slots(struct tetra_tmo_rx *rx, uint8_t mask)
{
    rx->slots = mask & TETRA_TS_ALL;
}

#if TETRA_ENABLE_CRYPTO
void tetra_tmo_set_key_provider(struct tetra_tmo_rx *rx,
                                tetra_tmo_key_provider provider, void *user)
{
    if (!rx)
        return;
    rx->key_provider = provider;
    rx->key_provider_user = user;
    for (unsigned i = 0; i < 4; i++) {
        rx->frag[i].active = false;
        memset(rx->frag[i].eck, 0, sizeof(rx->frag[i].eck));
    }
}

void tetra_tmo_set_cck_provider(struct tetra_tmo_rx *rx,
                                tetra_tmo_cck_provider provider, void *user)
{
    if (!rx)
        return;
    rx->cck_provider = provider;
    rx->cck_provider_user = user;
}
#endif /* TETRA_ENABLE_CRYPTO */

const struct tetra_cell *tetra_tmo_cell(const struct tetra_tmo_rx *rx)
{
    return rx->locked ? &rx->cell : NULL;
}

bool tetra_tmo_locked(const struct tetra_tmo_rx *rx)
{
    return rx->locked;
}

/* follow: ask to return to the main carrier when the lock is lost. */
static void set_lock(struct tetra_tmo_rx *rx, bool locked, bool follow)
{
    struct tetra_event ev;

    if (rx->locked == locked)
        return;
    rx->locked = locked;
    rx->realign_cand = false;
    rx->deaf_slots = 0;
    rx->crc_seen = rx->crc_ok;
#if TETRA_UPLINK_DETECT
    memset(&rx->ul, 0, sizeof(rx->ul));
#endif
#if TETRA_UPLINK_DECODE
    rx->uld.pending = 0;
#endif
    /* The uplink decoder reassembles in frag[0] while not locked. */
    for (unsigned i = 0; i < 4; i++) {
        rx->frag[i].active = false;
#if TETRA_ENABLE_CRYPTO
        memset(rx->frag[i].eck, 0, sizeof(rx->frag[i].eck));
#endif
    }
    if (!locked)
        memset(rx->dl_um, 0xff, sizeof(rx->dl_um));
    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_SYNC;
    ev.u.sync.locked = locked;
    tmo_emit(rx, &ev);
    if (!locked && follow)
        tmo_follow_lost(rx);
}

void tetra_tmo_resync(struct tetra_tmo_rx *rx)
{
    rx->cand = false;
    rx->follow.active = false;
    rx->follow.pend = NULL;
    rx->follow.tuned = 0xffff; /* the application chose the frequency */
    set_lock(rx, false, false);
}

void tmo_advance_time(struct tetra_tmo_rx *rx)
{
    if (++rx->t.tn > 4) {
        rx->t.tn = 1;
        tmo_frame_tick(rx);
        if (++rx->t.fn > 18) {
            rx->t.fn = 1;
            if (++rx->t.mn > 60) {
                rx->t.mn = 1;
                /* SYSINFO provides the current 16-bit hyperframe number.
                 * Keep it aligned with locally advanced TDMA time between
                 * subsequent SYSINFO updates. */
                if (rx->cell.have_hyperframe)
                    rx->cell.hyperframe++;
            }
        }
    }
}

/* Decode the BSCH of a synchronisation burst starting at slot index s. */
static bool decode_bsch(struct tetra_tmo_rx *rx, uint32_t s)
{
    int8_t *sb = rx->ws.in;

    rxcore_copy(&rx->core, s + TMO_SB_SB, sb, 120);
    if (!lmac_decode(&rx->ws, LMAC_SCH_S, sb, tetra_scramb_init(0), rx->blk[0]))
        return false;
    rx->crc_ok++;
    tmo_mac_sync(rx, rx->blk[0]);
    return true;
}

/* AACH: returns the downlink usage marker, or 0xff if unknown. */
static uint8_t decode_aach(struct tetra_tmo_rx *rx, uint32_t s, bool sb)
{
    int8_t bb[30];
    uint8_t hb[30];
    uint32_t cw = 0;
    uint16_t data;

    if (sb) {
        rxcore_copy(&rx->core, s + TMO_SB_BB, bb, 30);
    } else {
        rxcore_copy(&rx->core, s + TMO_NDB_BB1, bb, 14);
        rxcore_copy(&rx->core, s + TMO_NDB_BB2, bb + 14, 16);
    }
    lmac_descramble(&rx->ws, bb, 30, rx->scramb);
    tetra_soft_to_bits(bb, hb, 30);
    for (unsigned i = 0; i < 30; i++)
        cw = (cw << 1) | hb[i];
    if (tetra_rm3014_decode(cw, &data) < 0)
        return 0xff;

    /* ACCESS-ASSIGN, table 21.82: in frame 18 there is no DL usage. */
    unsigned header = (data >> 12) & 3;
    unsigned field1 = (data >> 6) & 0x3f;

    if (rx->t.fn == 18)
        return 0xfe;
    if (header == 0)
        return 2; /* common control */
    return (uint8_t)field1;
}

static bool decode_half(struct tetra_tmo_rx *rx, uint32_t pos, uint8_t *out)
{
    int8_t *b = rx->ws.in;

    rxcore_copy(&rx->core, pos, b, 216);
    if (!lmac_decode(&rx->ws, LMAC_SCH_H, b, rx->scramb, out))
        return false;
    rx->crc_ok++;
    return true;
}

static void process_traffic_full(struct tetra_tmo_rx *rx, uint32_t s,
                                 uint8_t um)
{
    rxcore_copy(&rx->core, s + TMO_NDB_BLK1, rx->tch, 216);
    rxcore_copy(&rx->core, s + TMO_NDB_BLK2, rx->tch + 216, 216);
    lmac_descramble(&rx->ws, rx->tch, TETRA_TCH_BITS, rx->scramb);
    tmo_traffic(rx, um, TETRA_HALF_TCH, TETRA_HALF_TCH);
}

static void process_stolen(struct tetra_tmo_rx *rx, uint32_t s, uint8_t um)
{
    enum tetra_half h1 = TETRA_HALF_STOLEN_C, h2 = TETRA_HALF_TCH;
    int fl = 0;
    bool ok1 = decode_half(rx, s + TMO_NDB_BLK1, rx->blk[0]);

    if (ok1) {
        /* MAC-U-SIGNAL starts with PDU type 11 */
        if ((rx->blk[0][0] >> 6) == 3)
            h1 = TETRA_HALF_STOLEN_U;
        fl = tmo_mac_block(rx, TMO_STCH, rx->blk[0], 124, 0);
    }

    /* If block 1 failed, try block 2 as STCH before assuming TCH. */
    bool ok2 = false;

    if (!ok1 || (fl & TMO_MAC_HALF2_STOLEN))
        ok2 = decode_half(rx, s + TMO_NDB_BLK2, rx->blk[1]);
    if (ok2) {
        h2 = (rx->blk[1][0] >> 6) == 3 ? TETRA_HALF_STOLEN_U
                                       : TETRA_HALF_STOLEN_C;
        tmo_mac_block(rx, TMO_STCH, rx->blk[1], 124, 216);
    }

    /* Hand the slot to the codec when any half holds traffic. */
    if (h2 == TETRA_HALF_TCH) {
        memset(rx->tch, 0, 216);
        rxcore_copy(&rx->core, s + TMO_NDB_BLK2, rx->tch + 216, 216);
        /* Block 2 alone is scrambled from its own first bit. */
        lmac_descramble(&rx->ws, rx->tch + 216, 216, rx->scramb);
        tmo_traffic(rx, um, h1, h2);
    }
}

static void process_slot(struct tetra_tmo_rx *rx, uint32_t s, bool sb,
                         bool train_n)
{
    uint8_t um;

    /* Deselected slots: only synchronisation bursts keep the timing. */
    if (!(rx->slots & (1u << (rx->t.tn - 1)))) {
        rx->dl_um[rx->t.tn - 1] = 0xff;
        rx->frag[rx->t.tn - 1].active = false;
#if TETRA_ENABLE_CRYPTO
        memset(rx->frag[rx->t.tn - 1].eck, 0,
               sizeof(rx->frag[rx->t.tn - 1].eck));
#endif
        if (sb)
            decode_bsch(rx, s);
        return;
    }
    um = decode_aach(rx, s, sb);
    bool traffic = um != 0xff && um != 0xfe && um >= 4;

    rx->dl_um[rx->t.tn - 1] = um;

    if (sb) {
        decode_bsch(rx, s);
        if (decode_half(rx, s + TMO_SB_BLK2, rx->blk[1]))
            tmo_mac_block(rx, TMO_SCH_HD, rx->blk[1], 124, 216);
        return;
    }

    if (train_n) {
        if (traffic) {
            process_traffic_full(rx, s, um);
            return;
        }
        int8_t *b = rx->ws.in;

        rxcore_copy(&rx->core, s + TMO_NDB_BLK1, b, 216);
        rxcore_copy(&rx->core, s + TMO_NDB_BLK2, b + 216, 216);
        if (lmac_decode(&rx->ws, LMAC_SCH_F, b, rx->scramb, rx->blk[0])) {
            rx->crc_ok++;
            tmo_mac_block(rx, TMO_SCH_F, rx->blk[0], 268, 0);
        }
        return;
    }

    if (traffic) {
        process_stolen(rx, s, um);
        return;
    }
    if (decode_half(rx, s + TMO_NDB_BLK1, rx->blk[0]))
        tmo_mac_block(rx, TMO_SCH_HD, rx->blk[0], 124, 0);
    if (decode_half(rx, s + TMO_NDB_BLK2, rx->blk[1]))
        tmo_mac_block(rx, TMO_SCH_HD, rx->blk[1], 124, 216);
}

/* Find the burst type and fine timing of the slot at rx->slot_start. */
static void locked_slot(struct tetra_tmo_rx *rx)
{
    int best = -2000, best_off = 0, kind = -1;

    for (unsigned i = 0; i < sizeof(search_order); i++) {
        int off = search_order[i];
        uint32_t s = rx->slot_start + (uint32_t)off;
        int cn, cp, cy;

        /* Timing usually holds: stop at a clear match. */
        if (best >= TRAIN_SURE)
            break;
        cn = rxcore_corr(&rx->core, s + TMO_NDB_TRAIN, tetra_seq_n, 22);
        cp = rxcore_corr(&rx->core, s + TMO_NDB_TRAIN, tetra_seq_p, 22);
        cy = rxcore_corr(&rx->core, s + TMO_SB_TRAIN, tetra_seq_y, 38);

        if (cn > best) {
            best = cn;
            best_off = off;
            kind = 0;
        }
        if (cp > best) {
            best = cp;
            best_off = off;
            kind = 1;
        }
        if (cy > best) {
            best = cy;
            best_off = off;
            kind = 2;
        }
    }

    if (best < TRAIN_MIN) {
        if (++rx->bad_slots >= LOCK_LOSS_SLOTS)
            set_lock(rx, false, true);
        return;
    }
    rx->bad_slots = 0;
    rx->slot_start += (uint32_t)best_off;
    process_slot(rx, rx->slot_start, kind == 2, kind == 0);
}

#if TETRA_UPLINK_DETECT || TETRA_UPLINK_DECODE
/* Uplink training sequences n, p and x as integers, first bit as MSB. */
#define UL_SEQ_N 0x343a74u
#define UL_SEQ_P 0x1e90deu
#define UL_SEQ_X 0x2743a743u

static unsigned seq_errors(uint32_t sr, uint32_t word, unsigned n)
{
    return tetra_popcount32((sr ^ word) & ((1u << n) - 1));
}

/*
 * n/p ending at bit count wr is part of a continuous downlink: q11..q22
 * starts 244 bits before it, q1 10 more.
 */
static bool np_in_downlink(const struct tetra_rxcore *c, uint32_t wr)
{
    return wr > 22 + 254 &&
           rxcore_corr(c, wr - 22 - 254, tetra_seq_q, 22) >= TRAIN_MIN;
}
#endif

#if TETRA_UPLINK_DETECT
/*
 * Uplink recognition, EN 300 392-2 clause 9.4.4. Evidence for an uplink is
 * the extended training sequence x of the control uplink burst, or normal
 * training sequences n/p a whole number of slots apart without the training
 * sequence q that a continuous downlink carries across slot boundaries.
 * The sync training sequence y (downlink, DMO) cancels the evidence.
 */
#define UL_MAX_SLOTS 8
#define UL_SLACK 4
/* Bits without y needed before reporting: two multiframes. */
#define UL_QUIET (2 * 18 * 4 * SLOT_BITS)
/* Bits without evidence after which the detection is forgotten: 10 s. */
#define UL_GAP 360000u
#define UL_SCORE 4

/* n/p hit ending at bit count wr: true if it looks like an uplink burst. */
static bool ul_np_hit(struct tetra_tmo_uldet *ul, const struct tetra_rxcore *c,
                      uint32_t wr)
{
    uint32_t last = ul->last_np, d;

    if (np_in_downlink(c, wr)) {
        ul->last_dl = wr;
        return false;
    }
    ul->last_np = wr;
    if (!last)
        return false;
    d = wr - last;
    if (d > UL_MAX_SLOTS * SLOT_BITS + UL_SLACK)
        return false;
    d = (d + UL_SLACK) % SLOT_BITS;
    return d <= 2 * UL_SLACK && wr - last > 2 * UL_SLACK;
}

static void ul_detect(struct tetra_tmo_rx *rx)
{
    struct tetra_tmo_uldet *ul = &rx->ul;
    uint32_t sr = (uint32_t)rx->core.hard_sr;
    uint32_t wr = rx->core.wr;
    bool hit = false;

    if (ul->score && wr - ul->last_ev > UL_GAP) {
        ul->score = 0;
        ul->reported = false;
    }
    if (rxcore_sync_hit(&rx->core))
        ul->last_dl = wr;
    else if (seq_errors(sr, UL_SEQ_X, 30) <= 2)
        hit = true;
    else if (seq_errors(sr, UL_SEQ_N, 22) <= 1 ||
             seq_errors(sr, UL_SEQ_P, 22) <= 1)
        hit = ul_np_hit(ul, &rx->core, wr);

    if (ul->last_dl == wr) {
        ul->score = 0;
        return;
    }
    if (!hit)
        return;
    ul->last_ev = wr;
    if (ul->score < UINT8_MAX)
        ul->score++;
    if (!ul->reported && ul->score >= UL_SCORE &&
        wr - ul->last_dl >= UL_QUIET) {
        struct tetra_event ev;

        ul->reported = true;
        memset(&ev, 0, sizeof(ev));
        ev.type = TETRA_EV_SIGNAL;
        ev.u.signal.type = TETRA_SIG_TMO_UPLINK;
        tmo_emit(rx, &ev);
    }
}
#endif

#if TETRA_UPLINK_DECODE
/*
 * Uplink burst decoding. Bursts of different MSs arrive with different
 * timing, so each one is aligned on its own training sequence: x for the
 * control uplink burst (SCH/HU), n or p for the normal uplink burst.
 */
enum { ULP_NONE = 0, ULP_CB, ULP_NUB_N, ULP_NUB_P, ULP_CB_OR_NUB };

/* Normal uplink bursts failing their CRC are taken for traffic only
 * within this many bits (10 s) of a good uplink block. */
#define UL_TRAFFIC_WINDOW 360000u

void tetra_tmo_set_uplink_cell(struct tetra_tmo_rx *rx, uint16_t mcc,
                               uint16_t mnc, uint8_t colour_code)
{
    rx->uld.scramb =
        tetra_scramb_init(tetra_tmo_colour_code(mcc, mnc, colour_code));
    rx->uld.manual = true;
    rx->uld.pending = ULP_NONE;
}

static void ul_traffic(struct tetra_tmo_rx *rx, enum tetra_half h1,
                       enum tetra_half h2)
{
    struct tetra_event ev;

    if (!rx->cfg.promiscuous)
        return; /* the talking party is not known */
    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_TRAFFIC;
    ev.u.traffic.half[0] = h1;
    ev.u.traffic.half[1] = h2;
    ev.u.traffic.bits = rx->tch;
    tmo_emit(rx, &ev);
}

/*
 * Tail bits of a normal uplink burst whose blocks start at b1 and b2: noise
 * often holds n or p, so traffic is only taken from complete bursts.
 */
static bool ul_tails_ok(const struct tetra_rxcore *c, uint32_t b1, uint32_t b2)
{
    unsigned err = 0;

    for (unsigned i = 0; i < 4; i++) {
        err += (rxcore_at(c, b1 - 4 + i) > 0) != tetra_tail[i];
        err += (rxcore_at(c, b2 + 216 + i) > 0) != tetra_tail[i];
    }
    return err <= 1;
}

static bool ul_decode_half(struct tetra_tmo_rx *rx, uint32_t pos, uint8_t *out)
{
    rxcore_copy(&rx->core, pos, rx->ws.in, 216);
    return lmac_decode(&rx->ws, LMAC_SCH_H, rx->ws.in, rx->uld.scramb, out);
}

/* Normal uplink burst with p: STCH in block 1, as on the downlink. */
static bool ul_stolen(struct tetra_tmo_rx *rx, uint32_t b1, uint32_t b2,
                      bool traffic_ok)
{
    enum tetra_half h1 = TETRA_HALF_STOLEN_C, h2 = TETRA_HALF_TCH;
    bool ok1 = ul_decode_half(rx, b1, rx->blk[0]), ok2 = false;
    int fl = 0;

    if (ok1) {
        if ((rx->blk[0][0] >> 6) == 3)
            h1 = TETRA_HALF_STOLEN_U;
        fl = tmo_ul_block(rx, TMO_UL_STCH, rx->blk[0], 124);
    }
    if (!ok1 || (fl & TMO_MAC_HALF2_STOLEN))
        ok2 = ul_decode_half(rx, b2, rx->blk[1]);
    if (ok2) {
        h2 = (rx->blk[1][0] >> 6) == 3 ? TETRA_HALF_STOLEN_U
                                       : TETRA_HALF_STOLEN_C;
        tmo_ul_block(rx, TMO_UL_STCH, rx->blk[1], 124);
    }
    if (h2 == TETRA_HALF_TCH &&
        (ok1 || (traffic_ok && ul_tails_ok(&rx->core, b1, b2)))) {
        memset(rx->tch, 0, 216);
        rxcore_copy(&rx->core, b2, rx->tch + 216, 216);
        lmac_descramble(&rx->ws, rx->tch + 216, 216, rx->uld.scramb);
        ul_traffic(rx, h1, h2);
    }
    return ok1 || ok2;
}

static __attribute__((noinline)) void ul_decode(struct tetra_tmo_rx *rx)
{
    struct tetra_tmo_ul *u = &rx->uld;
    uint32_t te = u->train_end, wr = rx->core.wr;
    int8_t *b = rx->ws.in;
    bool ok = false;
    bool traffic_ok = u->last_ok && wr - u->last_ok < UL_TRAFFIC_WINDOW;

    rx->in_uplink = true;
    if (u->pending == ULP_CB || u->pending == ULP_CB_OR_NUB) {
        /* For ULP_CB_OR_NUB train_end is the end of n: x ends 4 later. */
        uint32_t xe = u->pending == ULP_CB ? te : te + 4;
        uint32_t x = xe - TMO_CB_TRAIN;

        rxcore_copy(&rx->core, x - TMO_CB_HALF, b, TMO_CB_HALF);
        rxcore_copy(&rx->core, xe, b + TMO_CB_HALF, TMO_CB_HALF);
        ok = lmac_decode(&rx->ws, LMAC_SCH_HU, b, u->scramb, rx->blk[0]);
        if (ok)
            tmo_ul_block(rx, TMO_UL_SCH_HU, rx->blk[0], 92);
        else if (u->pending == ULP_CB_OR_NUB)
            u->pending = ULP_NUB_N; /* a normal burst after all */
    }
    if (u->pending == ULP_NUB_P) {
        ok = ul_stolen(rx, te - TMO_NUB_TRAIN - 216, te, traffic_ok);
    } else if (u->pending == ULP_NUB_N) {
        uint32_t b1 = te - TMO_NUB_TRAIN - 216, b2 = te;

        rxcore_copy(&rx->core, b1, b, 216);
        rxcore_copy(&rx->core, b2, b + 216, 216);
        ok = lmac_decode(&rx->ws, LMAC_SCH_F, b, u->scramb, rx->blk[0]);
        if (ok) {
            tmo_ul_block(rx, TMO_UL_SCH_F, rx->blk[0], 268);
        } else if (traffic_ok && ul_tails_ok(&rx->core, b1, b2)) {
            rxcore_copy(&rx->core, b1, rx->tch, 216);
            rxcore_copy(&rx->core, b2, rx->tch + 216, 216);
            lmac_descramble(&rx->ws, rx->tch, TETRA_TCH_BITS, u->scramb);
            ul_traffic(rx, TETRA_HALF_TCH, TETRA_HALF_TCH);
        }
    }
    rx->in_uplink = false;
    if (ok)
        u->last_ok = wr ? wr : 1;
    u->pending = ULP_NONE;
}

static void ul_search(struct tetra_tmo_rx *rx)
{
    struct tetra_tmo_ul *u = &rx->uld;
    uint32_t sr = (uint32_t)rx->core.hard_sr, wr = rx->core.wr;

    if (!u->scramb)
        return;
    /*
     * x holds n in its bits 5..26, so a control burst matches n 4 bits
     * before x is complete. Real normal bursts often carry bits around n
     * that complete an x: such a burst is tried as a control burst and,
     * if that fails, decoded as the normal burst, which ends later.
     */
    if (u->pending == ULP_NUB_N && wr - u->train_end == 4 &&
        seq_errors(sr, UL_SEQ_X, 30) <= 2) {
        u->pending = ULP_CB_OR_NUB;
        return;
    }
    if (u->pending) {
        if ((int32_t)(wr - u->due) >= 0)
            ul_decode(rx);
        return;
    }
    if (wr >= TMO_CB_TRAIN + TMO_CB_HALF && seq_errors(sr, UL_SEQ_X, 30) <= 2) {
        u->pending = ULP_CB;
        u->due = wr + TMO_CB_HALF;
        u->train_end = wr;
        return;
    }
    if (wr < TMO_NUB_TRAIN + 216 ||
        (seq_errors(sr, UL_SEQ_N, 22) > 1 &&
         seq_errors(sr, UL_SEQ_P, 22) > 1) ||
        np_in_downlink(&rx->core, wr))
        return;
    u->pending = rxcore_corr(&rx->core, wr - 22, tetra_seq_n, 22) >
                         rxcore_corr(&rx->core, wr - 22, tetra_seq_p, 22)
                     ? ULP_NUB_N
                     : ULP_NUB_P;
    u->due = wr + 216 + 4; /* block 2 and the tail bits */
    u->train_end = wr;
}
#endif

/* Not synchronised: look for the sync burst (and uplink bursts). */
static __attribute__((noinline)) void search(struct tetra_tmo_rx *rx)
{
#if TETRA_UPLINK_DETECT
    ul_detect(rx);
#endif
#if TETRA_UPLINK_DECODE
    ul_search(rx);
#endif
    if (!rx->cand && rxcore_sync_hit(&rx->core)) {
        /* y38 is the bit just received */
        rx->cand_start = rx->core.wr - (TMO_SB_TRAIN + 38);
        rx->cand = true;
    }
}

static __attribute__((noinline)) void cand_check(struct tetra_tmo_rx *rx)
{
    rx->cand = false;
    if (decode_bsch(rx, rx->cand_start)) {
        rx->slot_start = rx->cand_start;
        rx->bad_slots = 0;
        set_lock(rx, true, false);
        /* decode_bsch() set the time; handle the rest of the slot */
        process_slot(rx, rx->slot_start, true, false);
        rx->slot_start += SLOT_BITS;
        tmo_advance_time(rx);
    }
}

static __attribute__((noinline)) void slots_due(struct tetra_tmo_rx *rx)
{
    while (rx->locked &&
           rx->core.wr >= rx->slot_start + SLOT_BITS + 2 * TIMING_SEARCH) {
        locked_slot(rx);
        if (!rx->locked)
            break;
        rx->slot_start += SLOT_BITS;
        tmo_advance_time(rx);
        if (rx->crc_ok != rx->crc_seen) {
            rx->crc_seen = rx->crc_ok;
            rx->deaf_slots = 0;
        } else if (rx->deaf_slots < UINT16_MAX) {
            rx->deaf_slots++;
        }
    }
}

/*
 * Locked on a wrong slot grid. Once the timing is lost (a jump in the symbol
 * stream, from the transmitter or on the way), the training sequences still
 * match by chance every few slots at the +-TIMING_SEARCH offsets tried, so
 * LOCK_LOSS_SLOTS in a row without one never comes: the lock stays and no
 * block decodes, for good. Seen with a lab cell, after long SDS.
 *
 * So after REALIGN_DEAF_SLOTS without one block passing its CRC, also look
 * for the sync burst anywhere, as when not locked, and move the grid onto
 * one whose BSCH passes its CRC. A sync burst is only ever on the grid of
 * the cell sending it: one decoding where the grid does not is the cell's
 * real timing. Calls, fragments and the cell are kept.
 */
#define REALIGN_DEAF_SLOTS 8

static unsigned hyper_slot(const struct tetra_time *t)
{
    return ((unsigned)(t->mn - 1) * 18u + (unsigned)(t->fn - 1)) * 4u +
           (unsigned)(t->tn - 1);
}

static __attribute__((noinline)) void realign(struct tetra_tmo_rx *rx)
{
    if (!rx->realign_cand) {
        if (rxcore_sync_hit(&rx->core)) {
            rx->realign_start = rx->core.wr - (TMO_SB_TRAIN + 38);
            rx->realign_cand = true;
        }
        return;
    }
    if (rx->core.wr < rx->realign_start + SLOT_BITS + TIMING_SEARCH)
        return;
    rx->realign_cand = false;

    /* Where the grid has the slot the burst will name */
    const struct tetra_time old_t = rx->t;
    const uint32_t old_start = rx->slot_start;

    if (!decode_bsch(rx, rx->realign_start))
        return; /* sets rx->t to the burst's slot on success */
    int32_t slots = (int32_t)((hyper_slot(&rx->t) + 4320u -
                               hyper_slot(&old_t)) % 4320u);
    if (slots >= 2160)
        slots -= 4320;
    rx->realign_shift = (int32_t)(rx->realign_start -
                                  (old_start + (uint32_t)(slots * SLOT_BITS)));
    rx->realigned++;
    rx->slot_start = rx->realign_start;
    rx->bad_slots = 0;
    rx->deaf_slots = 0;
    rx->crc_seen = rx->crc_ok;
    /* decode_bsch() set the time; handle the rest of the slot */
    process_slot(rx, rx->slot_start, true, false);
    rx->slot_start += SLOT_BITS;
    tmo_advance_time(rx);
}

/* Per bit fast path. The rare work is kept out of line. */
static inline void push_one(struct tetra_tmo_rx *rx, int8_t s)
{
    rxcore_push(&rx->core, s);

    if (rx->locked) {
        if (rx->core.wr >= rx->slot_start + SLOT_BITS + 2 * TIMING_SEARCH)
            slots_due(rx);
        if (rx->locked && rx->deaf_slots >= REALIGN_DEAF_SLOTS)
            realign(rx);
        else
            rx->realign_cand = false;
        return;
    }
    search(rx);
    if (rx->cand && rx->core.wr >= rx->cand_start + SLOT_BITS + TIMING_SEARCH)
        cand_check(rx);
}

void tetra_tmo_push_soft(struct tetra_tmo_rx *rx, const int8_t *soft, size_t n)
{
    for (size_t i = 0; i < n; i++)
        push_one(rx, soft[i]);
}

void tetra_tmo_push_bits(struct tetra_tmo_rx *rx, const uint8_t *bits, size_t n)
{
    for (size_t i = 0; i < n; i++)
        push_one(rx, bits[i] ? TETRA_SOFT_ONE : TETRA_SOFT_ZERO);
}

void tetra_tmo_push_dibits(struct tetra_tmo_rx *rx, const uint8_t *dibits,
                           size_t n)
{
    for (size_t i = 0; i < n; i++) {
        push_one(rx, (dibits[i] & 2) ? TETRA_SOFT_ONE : TETRA_SOFT_ZERO);
        push_one(rx, (dibits[i] & 1) ? TETRA_SOFT_ONE : TETRA_SOFT_ZERO);
    }
}
