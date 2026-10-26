/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef DMO_SCENARIO_H
#define DMO_SCENARIO_H

/*
 * Synthetic DMO traffic for tests and the signal generator: builds DMAC
 * PDUs and DM bursts slot by slot with the libtetra TX building blocks.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called for every 510-bit slot; active is false for idle slots. */
typedef void (*scn_slot_cb)(void *user, const uint8_t bits[510], bool active);

struct dmo_scn {
    scn_slot_cb cb;
    void *user;
    uint32_t rng;
    uint8_t tn; /* 1..4 */
    uint8_t fn; /* 1..18 */
    uint32_t mni;
    uint32_t src;
    uint32_t dst;
};

/* Layer 2/3 description of one DMAC-SYNC / DMAC-DATA message. */
struct scn_msg {
    uint8_t msg_type;
    uint8_t frame_countdown;
    bool no_src;       /* DMAC-DATA without source address */
    const uint8_t *l3; /* packed bits after the message type */
    unsigned l3_bits;
};

void scn_init(struct dmo_scn *s, scn_slot_cb cb, void *user, uint32_t seed);

/* Emit an idle slot (random bits) and advance time. */
void scn_idle(struct dmo_scn *s);

/* Advance with idle slots until the given slot is next. */
void scn_goto(struct dmo_scn *s, uint8_t fn, uint8_t tn);

/* DSB with a non-fragmented DMAC-SYNC. */
void scn_dsb(struct dmo_scn *s, const struct scn_msg *m);

/* DSB heading a fragmented message; returns bits consumed from l3. */
unsigned scn_dsb_frag(struct dmo_scn *s, const struct scn_msg *m,
                      uint8_t nfrag);

/* SCH/F with DMAC-FRAG (end = false) or DMAC-END, from l3 at offset. */
unsigned scn_schf_frag(struct dmo_scn *s, const struct scn_msg *m,
                       unsigned offset, bool end);

/* DNB with a full traffic slot (432 type-4 bits). */
void scn_tch(struct dmo_scn *s, const uint8_t tch[432]);

/* DNB with STCH (DMAC-DATA) in the first half and half-slot TCH or a
 * second STCH message. */
void scn_stch(struct dmo_scn *s, const struct scn_msg *m1,
              const struct scn_msg *m2, const uint8_t tch_half[216]);

/* Deterministic pseudo random traffic content for frame fn. */
void scn_tch_pattern(uint8_t tch[432], unsigned seed);

/* Layer 3 element builders, returning the number of bits written. */
unsigned scn_l3_setup(uint8_t *buf, bool group, uint8_t prio);
unsigned scn_l3_tx_ceased(uint8_t *buf, uint8_t cause);
unsigned scn_l3_reserved(uint8_t *buf, uint8_t remaining);
unsigned scn_l3_info(uint8_t *buf, uint32_t mni, uint32_t ssi);
unsigned scn_l3_sds_status(uint8_t *buf, uint16_t status);
unsigned scn_l3_sds_text(uint8_t *buf, const char *latin1, bool fcs);
unsigned scn_l3_release(uint8_t *buf, uint8_t cause);

/*
 * Reference scenario, used by the tests and tetra_gen:
 *  1. idle channel
 *  2. group call 2002 -> 1001: DM-SETUP, traffic (DM-OCCUPIED in frames 6,
 *     12 and 18), one stolen slot with DM-INFO (TPNI), DM-TX CEASED
 *  3. reservation ending with DM-RESERVED (0 frames remaining)
 *  4. status SDS (single DSB), then a fragmented SDS-TL text message
 * Returns the number of traffic slots sent.
 */
#define SCN_TEXT "Hello from libtetra! This DMO text spans SCH/F fragments."
#define SCN_STATUS 32771
#define SCN_TPNI_SSI 4004
unsigned scn_run_reference(struct dmo_scn *s);

#ifdef __cplusplus
}
#endif

#endif /* DMO_SCENARIO_H */
