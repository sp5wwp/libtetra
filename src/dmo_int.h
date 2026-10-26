/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_DMO_INT_H
#define TETRA_DMO_INT_H

/* Internal interfaces between the DMO layers. */

#include <tetra/tetra_dmo.h>
#include "util/bits.h"

/* Logical channel of a block received in a DM normal burst. */
enum dmo_lchan {
    DMO_SCH_F = 0,
    DMO_STCH1, /* first half slot */
    DMO_STCH2, /* second half slot */
};

/* DM message types, EN 300 396-3 clause 9.3.19. */
enum dmo_msg {
    DM_RESERVED = 0,
    DM_SDS_OCCUPIED = 1,
    DM_TIMING_REQUEST = 2,
    DM_TIMING_ACK = 3,
    DM_SETUP = 8,
    DM_SETUP_PRES = 9,
    DM_CONNECT = 10,
    DM_DISCONNECT = 11,
    DM_CONNECT_ACK = 12,
    DM_OCCUPIED = 13,
    DM_RELEASE = 14,
    DM_TX_CEASED = 15,
    DM_TX_REQUEST = 16,
    DM_TX_ACCEPT = 17,
    DM_PREEMPT = 18,
    DM_PRE_ACCEPT = 19,
    DM_REJECT = 20,
    DM_INFO = 21,
    DM_SDS_UDATA = 22,
    DM_SDS_DATA = 23,
    DM_SDS_ACK = 24,
};

/* Flag returned by dmo_mac_dnb(): the second half slot is stolen. */
#define DMO_MAC_HALF2_STOLEN 1

/* Upper MAC (src/l2/dmo_mac.c) */
void dmo_mac_sync(struct tetra_dmo_rx *rx, const uint8_t *pdu);
int dmo_mac_dnb(struct tetra_dmo_rx *rx, enum dmo_lchan ch, const uint8_t *blk,
                unsigned nbits, bool crc_ok);
void dmo_frag_abort(struct tetra_dmo_rx *rx);

/* Layer 3 (src/l3/dmo_cc.c). buf[start] is the message type. */
void dmo_l3(struct tetra_dmo_rx *rx, const struct tetra_dmo_hdr *h,
            const uint8_t *buf, unsigned start, unsigned end, bool complete);
void dmo_traffic(struct tetra_dmo_rx *rx, enum tetra_half h1,
                 enum tetra_half h2);
void dmo_frame_tick(struct tetra_dmo_rx *rx);
void dmo_lost(struct tetra_dmo_rx *rx);

/* Helpers (src/dmo_rx.c) */
void dmo_emit(struct tetra_dmo_rx *rx, struct tetra_event *ev);
bool dmo_addressed(const struct tetra_dmo_rx *rx, uint32_t ssi);

#endif /* TETRA_DMO_INT_H */
