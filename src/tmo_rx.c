/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * TMO downlink receiver: synchronisation, TDMA tracking and slot dispatch.
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
/* Slots without a recognisable burst before declaring loss of lock. */
#define LOCK_LOSS_SLOTS 144

void tmo_emit(struct tetra_tmo_rx *rx, struct tetra_event *ev)
{
    ev->mode = TETRA_MODE_TMO;
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
    memset(rx->dl_um, 0xff, sizeof(rx->dl_um));
}

const struct tetra_cell *tetra_tmo_cell(const struct tetra_tmo_rx *rx)
{
    return rx->locked ? &rx->cell : NULL;
}

bool tetra_tmo_locked(const struct tetra_tmo_rx *rx)
{
    return rx->locked;
}

static void set_lock(struct tetra_tmo_rx *rx, bool locked)
{
    struct tetra_event ev;

    if (rx->locked == locked)
        return;
    rx->locked = locked;
#if TETRA_UPLINK_DETECT
    memset(&rx->ul, 0, sizeof(rx->ul));
#endif
    if (!locked) {
        memset(rx->dl_um, 0xff, sizeof(rx->dl_um));
        for (unsigned i = 0; i < 4; i++)
            rx->frag[i].active = false;
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_SYNC;
    ev.u.sync.locked = locked;
    tmo_emit(rx, &ev);
}

static void advance_time(struct tetra_tmo_rx *rx)
{
    if (++rx->t.tn > 4) {
        rx->t.tn = 1;
        tmo_frame_tick(rx);
        if (++rx->t.fn > 18) {
            rx->t.fn = 1;
            if (++rx->t.mn > 60)
                rx->t.mn = 1;
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
    tetra_descramble_soft(bb, 30, rx->scramb);
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
    return lmac_decode(&rx->ws, LMAC_SCH_H, b, rx->scramb, out);
}

static void process_traffic_full(struct tetra_tmo_rx *rx, uint32_t s,
                                 uint8_t um)
{
    rxcore_copy(&rx->core, s + TMO_NDB_BLK1, rx->tch, 216);
    rxcore_copy(&rx->core, s + TMO_NDB_BLK2, rx->tch + 216, 216);
    lmac_tch_descramble(rx->tch, TETRA_TCH_BITS, rx->scramb);
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
        fl = tmo_mac_block(rx, TMO_STCH, rx->blk[0], 124);
    }

    /* If block 1 failed, try block 2 as STCH before assuming TCH. */
    bool ok2 = false;

    if (!ok1 || (fl & TMO_MAC_HALF2_STOLEN))
        ok2 = decode_half(rx, s + TMO_NDB_BLK2, rx->blk[1]);
    if (ok2) {
        h2 = (rx->blk[1][0] >> 6) == 3 ? TETRA_HALF_STOLEN_U
                                       : TETRA_HALF_STOLEN_C;
        tmo_mac_block(rx, TMO_STCH, rx->blk[1], 124);
    }

    /* Hand the slot to the codec when any half holds traffic. */
    if (h2 == TETRA_HALF_TCH) {
        memset(rx->tch, 0, 216);
        rxcore_copy(&rx->core, s + TMO_NDB_BLK2, rx->tch + 216, 216);
        /* Block 2 alone is scrambled from its own first bit. */
        lmac_tch_descramble(rx->tch + 216, 216, rx->scramb);
        tmo_traffic(rx, um, h1, h2);
    }
}

static void process_slot(struct tetra_tmo_rx *rx, uint32_t s, bool sb,
                         bool train_n)
{
    uint8_t um = decode_aach(rx, s, sb);
    bool traffic = um != 0xff && um != 0xfe && um >= 4;

    rx->dl_um[rx->t.tn - 1] = um;

    if (sb) {
        decode_bsch(rx, s);
        if (decode_half(rx, s + TMO_SB_BLK2, rx->blk[1]))
            tmo_mac_block(rx, TMO_SCH_HD, rx->blk[1], 124);
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
        if (lmac_decode(&rx->ws, LMAC_SCH_F, b, rx->scramb, rx->blk[0]))
            tmo_mac_block(rx, TMO_SCH_F, rx->blk[0], 268);
        return;
    }

    if (traffic) {
        process_stolen(rx, s, um);
        return;
    }
    if (decode_half(rx, s + TMO_NDB_BLK1, rx->blk[0]))
        tmo_mac_block(rx, TMO_SCH_HD, rx->blk[0], 124);
    if (decode_half(rx, s + TMO_NDB_BLK2, rx->blk[1]))
        tmo_mac_block(rx, TMO_SCH_HD, rx->blk[1], 124);
}

/* Find the burst type and fine timing of the slot at rx->slot_start. */
static void locked_slot(struct tetra_tmo_rx *rx)
{
    int best = -2000, best_off = 0, kind = -1;

    for (int off = -TIMING_SEARCH; off <= TIMING_SEARCH; off += 2) {
        uint32_t s = rx->slot_start + (uint32_t)off;
        int cn = rxcore_corr(&rx->core, s + TMO_NDB_TRAIN, tetra_seq_n, 22);
        int cp = rxcore_corr(&rx->core, s + TMO_NDB_TRAIN, tetra_seq_p, 22);
        int cy = rxcore_corr(&rx->core, s + TMO_SB_TRAIN, tetra_seq_y, 38);

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
            set_lock(rx, false);
        return;
    }
    rx->bad_slots = 0;
    rx->slot_start += (uint32_t)best_off;
    process_slot(rx, rx->slot_start, kind == 2, kind == 0);
}

#if TETRA_UPLINK_DETECT
/*
 * Uplink recognition, EN 300 392-2 clause 9.4.4. Evidence for an uplink is
 * the extended training sequence x of the control uplink burst, or normal
 * training sequences n/p a whole number of slots apart without the training
 * sequence q that a continuous downlink carries across slot boundaries.
 * The sync training sequence y (downlink, DMO) cancels the evidence.
 */
#define UL_SEQ_N 0x343a74u
#define UL_SEQ_P 0x1e90deu
#define UL_SEQ_X 0x2743a743u
#define UL_MAX_SLOTS 8
#define UL_SLACK 4
/* Bits without y needed before reporting: two multiframes. */
#define UL_QUIET (2 * 18 * 4 * SLOT_BITS)
/* Bits without evidence after which the detection is forgotten: 10 s. */
#define UL_GAP 360000u
#define UL_SCORE 4

static unsigned seq_errors(uint64_t sr, uint32_t word, unsigned n)
{
    uint64_t d = (sr ^ word) & ((1ull << n) - 1);
    unsigned cnt = 0;

    while (d) {
        d &= d - 1;
        cnt++;
    }
    return cnt;
}

/* n/p hit ending at bit count wr: true if it looks like an uplink burst. */
static bool ul_np_hit(struct tetra_tmo_uldet *ul, const struct tetra_rxcore *c,
                      uint32_t wr)
{
    uint32_t start = wr - 22, last = ul->last_np, d;

    /* Continuous downlink: q11..q22 starts 244 bits before n/p, q1 10 more */
    if (wr > 22 + 254 &&
        rxcore_corr(c, start - 254, tetra_seq_q, 22) >= TRAIN_MIN) {
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
    uint64_t sr = rx->core.hard_sr;
    uint32_t wr = rx->core.wr;
    bool hit = false;

    if (ul->score && wr - ul->last_ev > UL_GAP) {
        ul->score = 0;
        ul->reported = false;
    }
    if (rxcore_sync_errors(&rx->core) <= TETRA_SYNC_MAX_ERRORS)
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

static void push_one(struct tetra_tmo_rx *rx, int8_t s)
{
    rxcore_push(&rx->core, s);

    if (!rx->locked) {
#if TETRA_UPLINK_DETECT
        ul_detect(rx);
#endif
        if (!rx->cand &&
            rxcore_sync_errors(&rx->core) <= TETRA_SYNC_MAX_ERRORS) {
            /* y38 is the bit just received */
            rx->cand_start = rx->core.wr - (TMO_SB_TRAIN + 38);
            rx->cand = true;
        }
        if (rx->cand &&
            rx->core.wr >= rx->cand_start + SLOT_BITS + TIMING_SEARCH) {
            rx->cand = false;
            if (decode_bsch(rx, rx->cand_start)) {
                rx->slot_start = rx->cand_start;
                rx->bad_slots = 0;
                set_lock(rx, true);
                /* decode_bsch() set the time; handle the rest of the slot */
                process_slot(rx, rx->slot_start, true, false);
                rx->slot_start += SLOT_BITS;
                advance_time(rx);
            }
        }
        return;
    }

    while (rx->locked &&
           rx->core.wr >= rx->slot_start + SLOT_BITS + 2 * TIMING_SEARCH) {
        locked_slot(rx);
        if (!rx->locked)
            break;
        rx->slot_start += SLOT_BITS;
        advance_time(rx);
    }
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
