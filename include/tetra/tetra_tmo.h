/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_TMO_H
#define TETRA_TMO_H

/*
 * TMO (V+D) downlink receiver, π/4-DQPSK continuous transmission.
 *
 * Feed it the demodulated bit stream of one carrier; it acquires the
 * synchronisation burst, follows the TDMA structure and reports cell
 * information, call control, short data and traffic through the event
 * callback. When TETRA_ENABLE_CRYPTO is enabled, air-interface encryption can
 * be decoded through an application key-provider callback. Otherwise encrypted
 * traffic/signalling is left opaque and reported as encrypted.
 *
 * While not synchronised, the receiver recognises TMO uplink transmissions
 * and reports them once with TETRA_EV_SIGNAL (TETRA_UPLINK_DETECT).
 */

#include <stddef.h>
#include <stdint.h>
#include <tetra/tetra_common.h>
#include <tetra/tetra_config.h>
#if TETRA_ENABLE_CRYPTO
#include <tetra/tetra_crypto.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Context. Allocate statically; fields are private. ---- */

enum tetra_tmo_encryption_mode {
    TETRA_TMO_ENC_CLEAR = 0,
    TETRA_TMO_ENC_RESERVED = 1,
    TETRA_TMO_ENC_ENCRYPTED_EVEN = 2,
    TETRA_TMO_ENC_ENCRYPTED_ODD = 3,
};

struct tetra_tmo_frag {
    bool active;
    bool encrypted;
    uint8_t encryption_mode; /* Raw two-bit MAC encryption mode. */
    uint8_t addr_type;
    uint8_t usage_marker;
    uint32_t addr;
    uint16_t nbits;
#if TETRA_ENABLE_CRYPTO
    bool crypto_valid;
    enum tetra_tea_algorithm algorithm;
    uint8_t eck[TETRA_TEA_KEY_BYTES];
#endif
    uint8_t buf[(TETRA_TMO_MAX_SDU_BITS + 7) / 8];
};

struct tetra_tmo_call_state {
    bool used;
    bool group;
    bool encrypted;
    bool connected;
    bool tx_active;
    uint8_t seen; /* set-up phase events already reported */
    uint8_t usage_marker;
    uint8_t timeslots;
    uint8_t circuit_type;
    uint8_t priority;
    uint16_t call_id;
    uint16_t carrier;
    uint16_t idle_frames;
    uint32_t dst_ssi;
    uint32_t talker_ssi;
#if TETRA_ENABLE_CRYPTO
    uint8_t encryption_mode;
    bool encryption_mode_valid;
#endif
};

struct tetra_tmo_uldet {
    bool reported;
    uint8_t score;
    uint32_t last_np; /* bit count at the last n/p hit, 0 = none */
    uint32_t last_ev; /* bit count at the last uplink evidence */
    uint32_t last_dl; /* bit count at the last downlink evidence */
};

#if TETRA_ENABLE_CRYPTO
/** Context passed to the application key provider. */
struct tetra_tmo_key_request {
    const struct tetra_cell *cell;
    uint32_t address;      /**< SSI/GSSI as received; may be ciphered, or 0. */
    uint8_t usage_marker;  /**< TMO usage marker, or 0 when unavailable. */
    bool traffic;          /**< True for traffic-channel decryption. */
    uint8_t encryption_mode; /**< Raw MAC ENC field: 2=even, 3=odd; 0 if unknown. */
    bool encryption_mode_valid; /**< Whether encryption_mode was observed. */
};

/**
 * Application-supplied TETRA base-key selector.
 *
 * The callback selects a provisioned 80-bit CK (typically the applicable
 * CCK/SCK) for the current cell and request, and returns the network KSG.
 * TB5 is then applied by libtetra. Return false when no matching key exists.
 * The request address is the on-air value and may itself be encrypted; key
 * selection must not assume that it is a plaintext SSI/GSSI. The CCK ID is
 * optional for key providers that select an SCK or other cell-level key.
 * The key is copied by the receiver and may be overwritten after return.
 */
typedef bool (*tetra_tmo_key_provider)(
    void *user, const struct tetra_tmo_key_request *request,
    enum tetra_tea_algorithm *algorithm,
    uint8_t ck[TETRA_TEA_KEY_BYTES]);

#endif /* TETRA_ENABLE_CRYPTO */

/* 353 KSS bits cover the second-half-only TCH/S allocation. */
#define TETRA_TCH_KSS_BITS 353

struct tetra_tmo_rx {
    struct tetra_rx_config cfg;
    struct tetra_rxcore core;
    struct tetra_lmac_ws ws;
    bool locked;
    bool cand;
    bool have_network;
    uint8_t slots;
    uint16_t bad_slots;
    uint32_t slot_start;
    uint32_t cand_start;
    struct tetra_time t;
    uint32_t scramb;
    struct tetra_cell cell;
    struct tetra_network network;
    uint8_t dl_um[4];
    uint8_t blk[2][34];
    int8_t tch[TETRA_TCH_BITS];
#if TETRA_ENABLE_CRYPTO
    uint8_t traffic_keystream[TETRA_TCH_KSS_BITS];
    tetra_tmo_key_provider key_provider;
    void *key_provider_user;
#endif
    struct tetra_tmo_frag frag[4];
    struct tetra_tmo_call_state calls[TETRA_TMO_MAX_CALLS];
#if TETRA_UPLINK_DETECT
    struct tetra_tmo_uldet ul;
#endif
};

/**
 * \brief Initialise a TMO receiver.
 *
 * @param rx: receiver context.
 * @param cfg: configuration (copied).
 */
void tetra_tmo_init(struct tetra_tmo_rx *rx, const struct tetra_rx_config *cfg);

/**
 * Configure the callback used to select provisioned base keys for TMO
 * decryption. Pass NULL to disable decryption. The callback may be changed
 * from an event callback; changes apply to subsequently processed blocks.
 * This API is available when TETRA_ENABLE_CRYPTO is enabled at build time.
 */
#if TETRA_ENABLE_CRYPTO
void tetra_tmo_set_key_provider(struct tetra_tmo_rx *rx,
                                tetra_tmo_key_provider provider, void *user);
#endif

/**
 * \brief Feed demodulated symbols as dibits.
 *
 * @param rx: receiver context.
 * @param dibits: one symbol per byte, B(2k-1) << 1 | B(2k).
 * @param n: number of symbols.
 */
void tetra_tmo_push_dibits(struct tetra_tmo_rx *rx, const uint8_t *dibits,
                           size_t n);

/**
 * \brief Feed soft bits.
 *
 * @param rx: receiver context.
 * @param soft: one value per bit, > 0 means '1'.
 * @param n: number of bits.
 */
void tetra_tmo_push_soft(struct tetra_tmo_rx *rx, const int8_t *soft, size_t n);

/**
 * \brief Feed hard bits.
 *
 * @param rx: receiver context.
 * @param bits: one bit per byte (0 or 1).
 * @param n: number of bits.
 */
void tetra_tmo_push_bits(struct tetra_tmo_rx *rx, const uint8_t *bits,
                         size_t n);

/**
 * \brief Select the timeslots whose signalling and traffic are decoded.
 *
 * Synchronisation bursts are always decoded. Deselecting slots saves CPU
 * time, for example decoding only the slots of a followed call, which are
 * reported in tetra_call.timeslots. Calls seen only on deselected slots
 * time out after a hyperframe. Takes effect from the next slot and may be
 * called from the event callback.
 *
 * @param rx: receiver context.
 * @param mask: TETRA_TS1..TETRA_TS4 combined, TETRA_TS_ALL by default.
 */
void tetra_tmo_set_slots(struct tetra_tmo_rx *rx, uint8_t mask);

/**
 * \brief Current cell information.
 *
 * @param rx: receiver context.
 * @return pointer to the cell data, or NULL if not synchronised yet.
 */
const struct tetra_cell *tetra_tmo_cell(const struct tetra_tmo_rx *rx);

/**
 * \brief Last network broadcast (D-NWRK-BROADCAST).
 *
 * @param rx: receiver context.
 * @return pointer to the data, or NULL if none was received yet.
 */
const struct tetra_network *tetra_tmo_network(const struct tetra_tmo_rx *rx);

/**
 * \brief Synchronisation state.
 *
 * @param rx: receiver context.
 * @return true while locked to the TDMA structure.
 */
bool tetra_tmo_locked(const struct tetra_tmo_rx *rx);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_TMO_H */
