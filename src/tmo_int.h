/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_TMO_INT_H
#define TETRA_TMO_INT_H

/* Internal interfaces between the TMO layers. */

#include <tetra/tetra_tmo.h>
#include "util/bits.h"

/* Logical channel a downlink MAC block was received on. */
enum tmo_lchan {
    TMO_SCH_F = 0,
    TMO_SCH_HD,
    TMO_STCH,
};

/* MAC address types, EN 300 392-2 table 21.55. */
enum tmo_addr_type {
    TMO_ADDR_NULL = 0,
    TMO_ADDR_SSI = 1,
    TMO_ADDR_EVENT = 2,
    TMO_ADDR_USSI = 3,
    TMO_ADDR_SMI = 4,
    TMO_ADDR_SSI_EVENT = 5,
    TMO_ADDR_SSI_UM = 6,
    TMO_ADDR_SMI_EVENT = 7,
};

/* Channel allocation element, EN 300 392-2 clause 21.5.2 (main fields). */
struct tmo_chan_alloc {
    uint8_t alloc_type;
    uint8_t timeslots; /* bitmap, bit 0 = TN1 */
    uint8_t ul_dl;
    uint16_t carrier;
};

/* MAC header information passed up with a TM-SDU. */
struct tmo_macinfo {
    uint8_t tn;
    uint8_t addr_type;
    uint8_t enc;
    uint8_t usage_marker; /* 0 if not assigned by this PDU */
    uint32_t ssi;
    bool has_ca;
    struct tmo_chan_alloc ca;
};

/* Flag returned by tmo_mac_block(): the second half slot is stolen. */
#define TMO_MAC_HALF2_STOLEN 1

/* Upper MAC (src/l2/tmo_mac.c) */
void tmo_mac_sync(struct tetra_tmo_rx *rx, const uint8_t *blk);
int tmo_mac_block(struct tetra_tmo_rx *rx, enum tmo_lchan ch,
                  const uint8_t *blk, unsigned nbits);

/* LLC, MLE and layer 3 (src/l3/tmo_l3.c) */
void tmo_llc(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
             const uint8_t *buf, unsigned start, unsigned end);
void tmo_traffic(struct tetra_tmo_rx *rx, uint8_t um, enum tetra_half h1,
                 enum tetra_half h2);
void tmo_frame_tick(struct tetra_tmo_rx *rx);

/* ---- Type 2 / type 3 element helpers (annex E) ---- */

struct opt {
    struct bitrd *b;
    bool o;
};

static inline void opt_begin(struct opt *o, struct bitrd *b)
{
    o->b = b;
    o->o = br_left(b) ? br_get(b, 1) : false;
}

/* P-bit of the next type 2 element. */
static inline bool opt_p(struct opt *o)
{
    if (!o->o || !br_left(o->b))
        return false;
    return br_get(o->b, 1);
}

/* Party address after a type identifier: 1 = SSI, 2 = SSI + extension. */
static inline uint32_t party_addr(struct bitrd *b, unsigned type, uint32_t *ext)
{
    uint32_t ssi = 0;

    if (type == 1 || type == 2)
        ssi = br_get(b, 24);
    if (type == 2) {
        uint32_t e = br_get(b, 24);

        if (ext)
            *ext = e;
    }
    return ssi;
}

/*
 * Next type 3/4 element after the type 2 elements: false if the M-bit says
 * none follows. On success *e is a reader limited to the element body and
 * b is advanced past it.
 */
static inline bool t34_next(struct bitrd *b, unsigned *id, struct bitrd *e)
{
    unsigned len;

    if (br_left(b) < 16 || !br_get(b, 1))
        return false;
    *id = br_get(b, 4);
    len = br_get(b, 11);
    if (b->err || len > br_left(b))
        return false;
    br_init(e, b->buf, b->pos, b->pos + len);
    br_skip(b, len);
    return true;
}

/* MLE and MM (src/l3/tmo_mle.c) */
/* MLE PDU type of D-NWRK-BROADCAST, table 18.85. */
#define MLE_D_NWRK_BROADCAST 2

/* Return false if the PDU was not decoded (reported as a raw PDU). */
bool tmo_mle_nwrk(struct tetra_tmo_rx *rx, struct bitrd *b);
bool tmo_mm(struct tetra_tmo_rx *rx, const struct tmo_macinfo *mi,
            struct bitrd *b);

/* Helpers (src/tmo_rx.c) */
void tmo_emit(struct tetra_tmo_rx *rx, struct tetra_event *ev);
bool tmo_addressed(const struct tetra_tmo_rx *rx, uint32_t ssi);

#endif /* TETRA_TMO_INT_H */
