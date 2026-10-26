/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_DMO_H
#define TETRA_DMO_H

/*
 * DMO (direct mode, EN 300 396) receiver for direct MS-MS operation.
 *
 * Feed it the demodulated bit stream of the DM carrier, including the
 * silence between bursts. It synchronises on DM synchronisation bursts,
 * tracks the channel state and reports calls, short data and traffic
 * through the event callback. Air interface encryption is not supported.
 */

#include <stddef.h>
#include <stdint.h>
#include <tetra/tetra_common.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Context. Allocate statically; fields are private. ---- */

/* Layer 2 header of a DMAC-SYNC or DMAC-DATA PDU. */
struct tetra_dmo_hdr {
    uint8_t comm_type;
    uint8_t aie;
    uint8_t frame_countdown;
    uint8_t dst_type;
    uint8_t src_type;
    uint8_t msg_type;
    bool frag;
    uint8_t nfrag;
    uint32_t dst;
    uint32_t src;
    uint32_t mni;
};

struct tetra_dmo_frag {
    bool active;
    bool started; /* first SCH/F expected */
    uint8_t remaining;
    struct tetra_dmo_hdr hdr;
    uint32_t dcc;
    uint16_t nbits;
    uint8_t buf[(TETRA_DMO_MAX_SDU_BITS + 7) / 8];
};

struct tetra_dmo_rx {
    struct tetra_rx_config cfg;
    struct tetra_rxcore core;
    struct tetra_lmac_ws ws;
    bool locked;
    bool cand;
    uint16_t idle_slots;
    uint32_t slot_start;
    uint32_t cand_start;
    struct tetra_time t;

    /* channel and call state */
    enum tetra_dmo_chan_state chan;
    uint32_t master_ssi;
    uint32_t master_mni;
    uint8_t priority;
    bool in_call;
    bool call_addressed;
    bool traffic;
    bool call_encrypted;
    bool call_group;
    uint8_t circuit_type;
    uint32_t call_dst;
    uint32_t talker;
    uint32_t dcc;
    bool have_dcc;
    uint16_t frames_no_occupied;
    uint16_t frames_idle;

    /* duplicate suppression */
    struct {
        bool valid;
        uint8_t msg_type;
        uint8_t frames_left;
        uint32_t src;
        uint32_t dst;
    } last;

    uint8_t blk[2][34];
    uint8_t sync_pdu[24]; /* SCH/S + SCH/H, 184 bits */
    int8_t tch[TETRA_TCH_BITS];
    struct tetra_dmo_frag frag;
};

/**
 * \brief Initialise a DMO receiver.
 *
 * @param rx: receiver context.
 * @param cfg: configuration (copied).
 */
void tetra_dmo_init(struct tetra_dmo_rx *rx, const struct tetra_rx_config *cfg);

/**
 * \brief Feed demodulated symbols as dibits.
 *
 * @param rx: receiver context.
 * @param dibits: one symbol per byte, B(2k-1) << 1 | B(2k).
 * @param n: number of symbols.
 */
void tetra_dmo_push_dibits(struct tetra_dmo_rx *rx, const uint8_t *dibits,
                           size_t n);

/**
 * \brief Feed soft bits.
 *
 * @param rx: receiver context.
 * @param soft: one value per bit, > 0 means '1'.
 * @param n: number of bits.
 */
void tetra_dmo_push_soft(struct tetra_dmo_rx *rx, const int8_t *soft, size_t n);

/**
 * \brief Feed hard bits.
 *
 * @param rx: receiver context.
 * @param bits: one bit per byte (0 or 1).
 * @param n: number of bits.
 */
void tetra_dmo_push_bits(struct tetra_dmo_rx *rx, const uint8_t *bits,
                         size_t n);

/**
 * \brief Current DM channel state.
 *
 * @param rx: receiver context.
 * @return channel state.
 */
enum tetra_dmo_chan_state tetra_dmo_channel(const struct tetra_dmo_rx *rx);

/**
 * \brief Synchronisation state.
 *
 * @param rx: receiver context.
 * @return true while a DM timing reference is held.
 */
bool tetra_dmo_locked(const struct tetra_dmo_rx *rx);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_DMO_H */
