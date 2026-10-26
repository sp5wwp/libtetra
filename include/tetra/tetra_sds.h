/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef TETRA_SDS_H
#define TETRA_SDS_H

/*
 * Short data helpers: text messaging and location information over SDS
 * type 4, with or without SDS-TL (EN 300 392-2 clause 29, EN 300 396-3
 * annex E).
 */

#include <stddef.h>
#include <stdint.h>
#include <tetra/tetra_common.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Protocol identifiers carrying text, table 29.21. */
#define TETRA_PID_SIMPLE_TEXT 0x02
#define TETRA_PID_SIMPLE_IMM_TEXT 0x09
#define TETRA_PID_TEXT 0x82
#define TETRA_PID_IMM_TEXT 0x89

/** Protocol identifiers carrying location information, table 29.21. */
#define TETRA_PID_SIMPLE_LOCATION 0x03
#define TETRA_PID_LIP 0x0a
#define TETRA_PID_LOCATION 0x83

/** Location system coding schemes, table 29.44. */
#define TETRA_LCS_NMEA 0x00
#define TETRA_LCS_RTCM 0x01

/** Text coding schemes, table 29.29 (most common). */
#define TETRA_TCS_GSM7 0x00
#define TETRA_TCS_LATIN1 0x01
#define TETRA_TCS_UCS2 0x1a

/** Parsed text message. */
struct tetra_sds_text {
    uint8_t protocol_id;
    bool sdstl;         /**< Carried in an SDS-TL SDS-TRANSFER. */
    bool immediate;     /**< Display immediately. */
    uint8_t msg_ref;    /**< SDS-TL message reference. */
    uint8_t report_req; /**< SDS-TL delivery report request. */
    uint8_t coding;     /**< Text coding scheme. */
    bool has_timestamp;
    uint32_t timestamp;  /**< Raw 24-bit time stamp. */
    const uint8_t *data; /**< Packed buffer holding the text. */
    uint16_t start;      /**< First bit of the text in data. */
    uint16_t nbits;      /**< Text length in bits. */
};

/**
 * \brief Recognise and parse a text message in an SDS event.
 *
 * @param sds: SDS event of type TETRA_SDS_UDD4.
 * @param t: parsed result; points into sds->data.
 * @return 0 if this is a text message, -1 otherwise.
 */
int tetra_sds_parse_text(const struct tetra_sds *sds, struct tetra_sds_text *t);

/**
 * \brief Convert the text to NUL terminated UTF-8.
 *
 * GSM 7-bit, ISO 8859-1 and UCS-2 are converted exactly; for other 8-bit
 * coding schemes ASCII is kept and other bytes become '?'.
 *
 * @param t: parsed text message.
 * @param out: output buffer.
 * @param len: size of the output buffer (> 0).
 * @return number of bytes written, excluding the terminating NUL.
 */
int tetra_sds_text_utf8(const struct tetra_sds_text *t, char *out, size_t len);

/** Parsed location system message (simple or over SDS-TL). */
struct tetra_sds_location {
    uint8_t protocol_id;
    bool sdstl;          /**< Carried in an SDS-TL SDS-TRANSFER. */
    uint8_t msg_ref;     /**< SDS-TL message reference. */
    uint8_t report_req;  /**< SDS-TL delivery report request. */
    uint8_t coding;      /**< TETRA_LCS_NMEA, TETRA_LCS_RTCM, ... */
    const uint8_t *data; /**< Packed buffer holding the location data. */
    uint16_t start;      /**< First bit of the location data in data. */
    uint16_t nbits;      /**< Location data length in bits. */
};

/**
 * \brief Recognise and parse a location system message (protocol
 * identifiers 0x03 and 0x83) in an SDS event.
 *
 * @param sds: SDS event of type TETRA_SDS_UDD4.
 * @param l: parsed result; points into sds->data.
 * @return 0 if this is a location system message, -1 otherwise.
 */
int tetra_sds_parse_location(const struct tetra_sds *sds,
                             struct tetra_sds_location *l);

/**
 * \brief Copy NMEA location data as NUL terminated ASCII.
 *
 * Bytes other than printable ASCII, CR and LF are dropped.
 *
 * @param l: parsed location system message.
 * @param out: output buffer.
 * @param len: size of the output buffer (> 0).
 * @return number of bytes written, excluding the terminating NUL.
 */
int tetra_sds_location_ascii(const struct tetra_sds_location *l, char *out,
                             size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_SDS_H */
