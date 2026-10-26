/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_COMMON_H
#define TETRA_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tetra/tetra_config.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Air interface mode an event originates from. */
enum tetra_mode {
    TETRA_MODE_TMO = 0, /**< Trunked mode, V+D downlink. */
    TETRA_MODE_DMO = 1, /**< Direct mode. */
};

/** Event types delivered to the application callback. */
enum tetra_event_type {
    TETRA_EV_SYNC = 0, /**< Burst synchronisation acquired or lost. */
    TETRA_EV_CELL,     /**< TMO cell information (SYNC / SYSINFO). */
    TETRA_EV_CHANNEL,  /**< DMO channel state change. */
    TETRA_EV_CALL,     /**< Call control. */
    TETRA_EV_SDS,      /**< Short data or status message. */
    TETRA_EV_TRAFFIC,  /**< One traffic slot for the speech codec. */
    TETRA_EV_PDU,      /**< Other layer 2/3 PDU, for monitoring. */
    TETRA_EV_SIGNAL,   /**< Recognised signal that cannot be decoded. */
};

/** Position in the TDMA structure. */
struct tetra_time {
    uint8_t tn; /**< Timeslot number 1..4. */
    uint8_t fn; /**< Frame number 1..18. */
    uint8_t mn; /**< Multiframe number 1..60 (TMO only, 0 for DMO). */
};

/** Synchronisation event. */
struct tetra_sync {
    bool locked;
};

/** Signal types reported by TETRA_EV_SIGNAL. */
enum tetra_signal_type {
    TETRA_SIG_TMO_UPLINK = 0, /**< TMO uplink (MS to BS) transmissions. */
};

/** Signal recognition event. */
struct tetra_signal {
    enum tetra_signal_type type;
};

/** TMO cell information, refreshed by SYNC and SYSINFO. */
struct tetra_cell {
    uint16_t mcc;
    uint16_t mnc;
    uint8_t colour_code;
    uint8_t system_code;
    uint8_t sharing_mode;
    bool have_sysinfo; /**< Fields below are valid. */
    uint16_t location_area;
    uint16_t main_carrier;
    uint8_t band;
    uint8_t offset;         /**< 0: 0 Hz, 1: +6.25k, 2: -6.25k, 3: +12.5k. */
    uint8_t duplex_spacing; /**< Raw code, see TS 100 392-15. */
    bool reverse_operation;
    uint32_t dl_freq_hz;      /**< Downlink frequency of the main carrier. */
    uint16_t hyperframe;      /**< Last broadcast hyperframe number. */
    uint16_t cck_id;          /**< Last broadcast CCK identifier. */
    uint16_t service_details; /**< BS service details, 12 bits. */
    bool air_encryption;      /**< Cell uses air interface encryption. */
};

/** DMO channel states, EN 300 396-3 clause 8.4.2. */
enum tetra_dmo_chan_state {
    TETRA_DMO_FREE = 0,
    TETRA_DMO_OCCUPIED,
    TETRA_DMO_RESERVED,
    TETRA_DMO_SDS_OCCUPIED,
};

/** DMO channel event. */
struct tetra_channel {
    enum tetra_dmo_chan_state state;
    uint32_t master_ssi; /**< Source address of the current master. */
    uint32_t mni;
    uint8_t priority;
};

/** Call control events, common to TMO and DMO. */
enum tetra_call_what {
    TETRA_CALL_SETUP = 0,  /**< New call / call set-up. */
    TETRA_CALL_CONNECT,    /**< Call connected (presence check calls). */
    TETRA_CALL_TX_GRANTED, /**< A party started transmitting. */
    TETRA_CALL_TX_CEASED,  /**< Current transmission ended. */
    TETRA_CALL_LATE_ENTRY, /**< Ongoing call joined (DMO DM-OCCUPIED). */
    TETRA_CALL_INFO,       /**< Talking party identification update. */
    TETRA_CALL_RELEASE,    /**< Call released or lost. */
};

/** Call control event. Address fields are 0 when unknown. */
struct tetra_call {
    enum tetra_call_what what;
    uint16_t call_id;     /**< TMO call identifier, 0 for DMO. */
    uint8_t usage_marker; /**< TMO traffic usage marker, 0 if none. */
    uint8_t timeslots;    /**< TMO assigned timeslots, bit 0 = TN1. */
    uint16_t carrier;     /**< TMO allocated carrier, 0xffff = none. */
    uint32_t dst_ssi;     /**< Called party (group or individual). */
    uint32_t src_ssi;     /**< Calling or talking party. */
    uint32_t mni;         /**< DMO MNI from the MAC header. */
    bool group;           /**< Group call (when known). */
    bool encrypted;       /**< End-to-end or air encryption flagged. */
    bool addressed;       /**< Addressed to one of our identities. */
    uint8_t priority;     /**< Call priority (TMO 0..15, DMO 0..3). */
    uint8_t circuit_type; /**< 0 = speech, other values = data. */
    uint8_t cause;        /**< Release / disconnect / cease cause. */
};

/** Short data payload type. */
enum tetra_sds_type {
    TETRA_SDS_UDD1 = 0, /**< 16-bit user defined data. */
    TETRA_SDS_UDD2,     /**< 32-bit user defined data. */
    TETRA_SDS_UDD3,     /**< 64-bit user defined data. */
    TETRA_SDS_UDD4,     /**< Variable length, starts with protocol id. */
    TETRA_SDS_STATUS,   /**< 16-bit pre-coded status. */
};

/** Short data event. */
struct tetra_sds {
    enum tetra_sds_type type;
    uint32_t src_ssi;
    uint32_t src_ext; /**< Source MNI if a TSI was given, else 0. */
    uint32_t dst_ssi;
    bool addressed; /**< Addressed to one of our identities. */
    bool complete;  /**< false if part of the data was lost. */
    bool fcs_ok;    /**< DMO: FCS present and correct. */
    bool has_fcs;
    uint16_t status;     /**< Pre-coded status value. */
    const uint8_t *data; /**< Packed payload (UDD1..4), MSB first. */
    uint16_t nbits;      /**< Payload length in bits. */
};

/** Content of one half of a traffic slot. */
enum tetra_half {
    TETRA_HALF_TCH = 0,  /**< Traffic. */
    TETRA_HALF_STOLEN_C, /**< Stolen for C-plane signalling. */
    TETRA_HALF_STOLEN_U, /**< Stolen for U-plane signalling. */
};

/** Number of type-4 bits in a full traffic slot. */
#define TETRA_TCH_BITS 432

/**
 * Traffic slot for the speech codec.
 *
 * bits holds the 432 descrambled type-4 soft bits of the slot as defined in
 * EN 300 395-2 (> 0 means '1'). If the first half was stolen, only
 * bits[216..431] are traffic: they carry a half slot (216,101)-interleaved
 * speech frame.
 */
struct tetra_traffic {
    struct tetra_time time;
    uint8_t usage_marker; /**< TMO usage marker. */
    uint16_t call_id;     /**< TMO call identifier if known. */
    uint32_t talker_ssi;  /**< Current talking party if known. */
    uint8_t circuit_type; /**< 0 = speech. */
    bool encrypted;       /**< Traffic is encrypted: not decodable. */
    enum tetra_half half[2];
    const int8_t *bits;
};

/** Protocol of a monitored PDU. */
enum tetra_proto {
    TETRA_PROTO_MM = 1,
    TETRA_PROTO_CMCE = 2,
    TETRA_PROTO_SNDCP = 4,
    TETRA_PROTO_MLE = 5,
    TETRA_PROTO_MGMT = 6,
    TETRA_PROTO_DMCC = 16, /**< DMO layer 3 / layer 2 message. */
    TETRA_PROTO_LLC = 17,  /**< LLC PDU not handled further. */
};

/** Monitored PDU (anything not reported by a more specific event). */
struct tetra_pdu {
    enum tetra_proto proto;
    uint8_t type;        /**< Protocol specific PDU type. */
    uint32_t addr;       /**< MAC layer address (destination). */
    bool encrypted;      /**< Content is encrypted, not parsed. */
    const uint8_t *data; /**< Packed PDU, starting at the PDU type. */
    uint16_t nbits;
};

/** Event delivered to the application. Valid only during the callback. */
struct tetra_event {
    enum tetra_event_type type;
    enum tetra_mode mode;
    struct tetra_time time;
    union {
        struct tetra_sync sync;
        const struct tetra_cell *cell;
        struct tetra_channel channel;
        struct tetra_call call;
        struct tetra_sds sds;
        struct tetra_traffic traffic;
        struct tetra_pdu pdu;
        struct tetra_signal signal;
    } u;
};

/**
 * \brief Application event callback.
 *
 * Called from the context that pushes bits into the receiver. It should
 * return quickly; any data referenced by the event must be copied.
 *
 * @param user: opaque pointer from the configuration.
 * @param ev: event.
 */
typedef void (*tetra_event_cb)(void *user, const struct tetra_event *ev);

/** Receiver configuration, common to TMO and DMO. */
struct tetra_rx_config {
    tetra_event_cb cb;
    void *user;
    uint32_t issi;          /**< Own individual SSI, 0 if none. */
    uint32_t mni;           /**< Own MNI (MCC << 14 | MNC), DMO only. */
    const uint32_t *groups; /**< Own group SSIs. */
    uint8_t ngroups;
    bool promiscuous; /**< Report everything, not only own calls. */
};

/* ---- Internal state shared by the receivers. Treat as opaque. ---- */

/**
 * Soft bit history. Must be a power of two and hold a slot (510 bits) plus
 * the timing search window, so 1024 is the smallest valid size.
 */
#define TETRA_RING_BITS 1024

#ifdef __cplusplus
static_assert((TETRA_RING_BITS & (TETRA_RING_BITS - 1)) == 0,
              "TETRA_RING_BITS must be a power of two");
static_assert(TETRA_RING_BITS >= 1024, "TETRA_RING_BITS too small");
#else
_Static_assert((TETRA_RING_BITS & (TETRA_RING_BITS - 1)) == 0,
               "TETRA_RING_BITS must be a power of two");
_Static_assert(TETRA_RING_BITS >= 1024, "TETRA_RING_BITS too small");
#endif

struct tetra_rxcore {
    int8_t ring[TETRA_RING_BITS];
    uint32_t wr;      /* total number of bits received */
    uint64_t hard_sr; /* last 64 hard bits, newest in bit 0 */
};

struct tetra_lmac_ws {
    int8_t in[432];
    int8_t t5[432];
    int8_t t3[432];
    int8_t mother[4 * 292];
    uint8_t t2[292];
    uint16_t dec[292];
};

/**
 * \brief Downlink carrier frequency.
 *
 * @param band: frequency band (base = band x 100 MHz).
 * @param carrier: carrier number (25 kHz steps).
 * @param offset: offset code (0..3).
 * @return frequency in Hz.
 */
uint32_t tetra_carrier_hz(uint8_t band, uint16_t carrier, uint8_t offset);

/**
 * \brief Map a traffic event onto the 690-word frame used by the ETSI
 * reference channel decoder (as written by osmo-tetra).
 *
 * The block holds six headers 0x6b21 + i every 115 words and the 432 soft
 * bits with '1' mapped to negative values. When the first half was stolen,
 * only the 216 bits of block 2 are copied, starting at the first position.
 *
 * @param t: traffic event.
 * @param out: 690 output words.
 */
void tetra_traffic_to_etsi(const struct tetra_traffic *t, int16_t out[690]);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_COMMON_H */
