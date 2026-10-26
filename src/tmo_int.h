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

/* Helpers (src/tmo_rx.c) */
void tmo_emit(struct tetra_tmo_rx *rx, struct tetra_event *ev);
bool tmo_addressed(const struct tetra_tmo_rx *rx, uint32_t ssi);

#endif /* TETRA_TMO_INT_H */
