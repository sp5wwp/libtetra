/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * DMO receiver: burst synchronisation and slot dispatch.
 * EN 300 396-2 clauses 7 and 9, EN 300 396-3 clause 8.2.
 */

#include <string.h>
#include <tetra/tetra_coding.h>
#include <tetra/tetra_dmo.h>
#include "dmo_int.h"
#include "l1/l1.h"
#include "l2/lmac.h"

/*
 * Correlation (per mille) needed to accept a training sequence. The DNB
 * sequences are short (22 bits) and DMO has idle slots full of noise, so
 * they need a stronger match than the 38-bit sync sequence.
 */
#define TRAIN_MIN_SYNC 600
#define TRAIN_MIN_NORMAL 750
#define TIMING_SEARCH 4
/* Empty slots before the timing reference is dropped (about 4 s). */
#define LOCK_LOSS_SLOTS 288

void dmo_emit(struct tetra_dmo_rx *rx, struct tetra_event *ev)
{
    ev->mode = TETRA_MODE_DMO;
    ev->time = rx->t;
    ev->time.mn = 0;
    if (rx->cfg.cb)
        rx->cfg.cb(rx->cfg.user, ev);
}

bool dmo_addressed(const struct tetra_dmo_rx *rx, uint32_t ssi)
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

void tetra_dmo_init(struct tetra_dmo_rx *rx, const struct tetra_rx_config *cfg)
{
    memset(rx, 0, sizeof(*rx));
    rx->cfg = *cfg;
    rx->t.tn = 1;
    rx->t.fn = 1;
}

enum tetra_dmo_chan_state tetra_dmo_channel(const struct tetra_dmo_rx *rx)
{
    return rx->chan;
}

bool tetra_dmo_locked(const struct tetra_dmo_rx *rx)
{
    return rx->locked;
}

static void set_lock(struct tetra_dmo_rx *rx, bool locked)
{
    struct tetra_event ev;

    if (rx->locked == locked)
        return;
    rx->locked = locked;
    if (!locked)
        dmo_lost(rx);
    memset(&ev, 0, sizeof(ev));
    ev.type = TETRA_EV_SYNC;
    ev.u.sync.locked = locked;
    dmo_emit(rx, &ev);
}

static void advance_time(struct tetra_dmo_rx *rx)
{
    if (++rx->t.tn > 4) {
        rx->t.tn = 1;
        if (++rx->t.fn > 18)
            rx->t.fn = 1;
        dmo_frame_tick(rx);
    }
}

static bool decode_sch_s(struct tetra_dmo_rx *rx, uint32_t s)
{
    int8_t *sb = rx->ws.in;

    rxcore_copy(&rx->core, s + DMO_DSB_SB, sb, 120);
    return lmac_decode(&rx->ws, LMAC_SCH_S, sb, tetra_scramb_init(0),
                       rx->blk[0]);
}

static void process_dsb(struct tetra_dmo_rx *rx, uint32_t s)
{
    int8_t *b = rx->ws.in;

    if (!decode_sch_s(rx, s))
        return;
    rxcore_copy(&rx->core, s + DMO_DSB_BLK2, b, 216);
    if (!lmac_decode(&rx->ws, LMAC_SCH_H, b, tetra_scramb_init(0), rx->blk[1]))
        return;
    /* DMAC-SYNC spans SCH/S (60 bits) and SCH/H (124 bits). */
    memset(rx->sync_pdu, 0, sizeof(rx->sync_pdu));
    bits_copy(rx->sync_pdu, 0, rx->blk[0], 0, 60);
    bits_copy(rx->sync_pdu, 60, rx->blk[1], 0, 124);
    dmo_mac_sync(rx, rx->sync_pdu);
}

static uint32_t dnb_scramb(const struct tetra_dmo_rx *rx, bool sch_f)
{
    if (sch_f && rx->frag.active)
        return tetra_scramb_init(rx->frag.dcc);
    return tetra_scramb_init(rx->dcc);
}

static bool decode_half(struct tetra_dmo_rx *rx, uint32_t pos, uint8_t *out)
{
    int8_t *b = rx->ws.in;

    rxcore_copy(&rx->core, pos, b, 216);
    return lmac_decode(&rx->ws, LMAC_SCH_H, b, dnb_scramb(rx, false), out);
}

static void process_dnb(struct tetra_dmo_rx *rx, uint32_t s, bool train_n)
{
    if (!rx->have_dcc && !rx->frag.active)
        return; /* no colour code: cannot descramble */

    if (train_n) {
        if (rx->traffic && !rx->frag.active) {
            rxcore_copy(&rx->core, s + DMO_DNB_BLK1, rx->tch, 216);
            rxcore_copy(&rx->core, s + DMO_DNB_BLK2, rx->tch + 216, 216);
            tetra_descramble_soft(rx->tch, TETRA_TCH_BITS,
                                  tetra_scramb_init(rx->dcc));
            dmo_traffic(rx, TETRA_HALF_TCH, TETRA_HALF_TCH);
            return;
        }
        int8_t *b = rx->ws.in;
        bool ok;

        rxcore_copy(&rx->core, s + DMO_DNB_BLK1, b, 216);
        rxcore_copy(&rx->core, s + DMO_DNB_BLK2, b + 216, 216);
        ok = lmac_decode(&rx->ws, LMAC_SCH_F, b, dnb_scramb(rx, true),
                         rx->blk[0]);
        dmo_mac_dnb(rx, DMO_SCH_F, rx->blk[0], 268, ok);
        return;
    }

    /* Normal training sequence 2: the first half slot is STCH. */
    enum tetra_half h1 = TETRA_HALF_STOLEN_C, h2 = TETRA_HALF_TCH;
    bool ok1 = decode_half(rx, s + DMO_DNB_BLK1, rx->blk[0]);
    bool ok2 = false;
    int fl = 0;

    if (ok1) {
        if ((rx->blk[0][0] >> 6) == 3)
            h1 = TETRA_HALF_STOLEN_U;
        fl = dmo_mac_dnb(rx, DMO_STCH1, rx->blk[0], 124, true);
    }
    if (!ok1 || (fl & DMO_MAC_HALF2_STOLEN) || !rx->traffic) {
        ok2 = decode_half(rx, s + DMO_DNB_BLK2, rx->blk[1]);
        if (ok2) {
            h2 = (rx->blk[1][0] >> 6) == 3 ? TETRA_HALF_STOLEN_U
                                           : TETRA_HALF_STOLEN_C;
            dmo_mac_dnb(rx, DMO_STCH2, rx->blk[1], 124, true);
        } else if (fl & DMO_MAC_HALF2_STOLEN) {
            /* Stolen but undecodable: drop a pending STCH fragment. */
            dmo_mac_dnb(rx, DMO_STCH2, rx->blk[1], 124, false);
            h2 = TETRA_HALF_STOLEN_C;
        }
    }
    if (rx->traffic && h2 == TETRA_HALF_TCH) {
        memset(rx->tch, 0, 216);
        rxcore_copy(&rx->core, s + DMO_DNB_BLK2, rx->tch + 216, 216);
        tetra_descramble_soft(rx->tch + 216, 216, tetra_scramb_init(rx->dcc));
        dmo_traffic(rx, h1, h2);
    }
}

static void locked_slot(struct tetra_dmo_rx *rx)
{
    int best = -2000, best_off = 0, kind = -1;

    /*
     * DM normal bursts only occur in slot 1 (EN 300 396-2 table 21; on
     * channel B the master's own slot numbering applies).
     */
    bool dnb_slot = rx->t.tn == 1;

    for (int off = -TIMING_SEARCH; off <= TIMING_SEARCH; off += 2) {
        uint32_t s = rx->slot_start + (uint32_t)off;
        int cn = -2000, cp = -2000;
        int cy = rxcore_corr(&rx->core, s + DMO_DSB_TRAIN, tetra_seq_y, 38);

        if (dnb_slot) {
            cn = rxcore_corr(&rx->core, s + DMO_DNB_TRAIN, tetra_seq_n, 22);
            cp = rxcore_corr(&rx->core, s + DMO_DNB_TRAIN, tetra_seq_p, 22);
            if (cn < TRAIN_MIN_NORMAL)
                cn = -2000;
            if (cp < TRAIN_MIN_NORMAL)
                cp = -2000;
        }
        if (cy < TRAIN_MIN_SYNC)
            cy = -2000;

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

    if (kind < 0) {
        if (++rx->idle_slots >= LOCK_LOSS_SLOTS)
            set_lock(rx, false);
        return;
    }
    rx->idle_slots = 0;
    rx->frames_idle = 0;
    rx->slot_start += (uint32_t)best_off;
    if (kind == 2)
        process_dsb(rx, rx->slot_start);
    else
        process_dnb(rx, rx->slot_start, kind == 0);
}

/* True if a burst starting at s does not fit the current slot timing. */
static bool misaligned(const struct tetra_dmo_rx *rx, uint32_t s)
{
    int32_t d = (int32_t)(s - rx->slot_start) % SLOT_BITS;

    if (d < 0)
        d += SLOT_BITS;
    return d > TIMING_SEARCH && d < SLOT_BITS - TIMING_SEARCH;
}

static void push_one(struct tetra_dmo_rx *rx, int8_t s)
{
    rxcore_push(&rx->core, s);

    if (!rx->cand && rxcore_sync_errors(&rx->core) <= TETRA_SYNC_MAX_ERRORS) {
        uint32_t cs = rx->core.wr - (DMO_DSB_TRAIN + 38);

        if (!rx->locked || misaligned(rx, cs)) {
            rx->cand_start = cs;
            rx->cand = true;
        }
    }
    if (rx->cand &&
        rx->core.wr >= rx->cand_start + SLOT_BITS + 2 * TIMING_SEARCH) {
        rx->cand = false;
        if (decode_sch_s(rx, rx->cand_start)) {
            /* New timing reference: the locked loop handles the slot. */
            rx->slot_start = rx->cand_start;
            rx->idle_slots = 0;
            set_lock(rx, true);
        }
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

void tetra_dmo_push_soft(struct tetra_dmo_rx *rx, const int8_t *soft, size_t n)
{
    for (size_t i = 0; i < n; i++)
        push_one(rx, soft[i]);
}

void tetra_dmo_push_bits(struct tetra_dmo_rx *rx, const uint8_t *bits, size_t n)
{
    for (size_t i = 0; i < n; i++)
        push_one(rx, bits[i] ? TETRA_SOFT_ONE : TETRA_SOFT_ZERO);
}

void tetra_dmo_push_dibits(struct tetra_dmo_rx *rx, const uint8_t *dibits,
                           size_t n)
{
    for (size_t i = 0; i < n; i++) {
        push_one(rx, (dibits[i] & 2) ? TETRA_SOFT_ONE : TETRA_SOFT_ZERO);
        push_one(rx, (dibits[i] & 1) ? TETRA_SOFT_ONE : TETRA_SOFT_ZERO);
    }
}
