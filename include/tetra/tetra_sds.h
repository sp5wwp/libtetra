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

/* ---- Location Information Protocol, ETSI TS 100 392-18-1 ---- */

/** LIP PDU kinds. */
enum tetra_lip_kind {
    TETRA_LIP_SHORT_REPORT = 0, /**< SHORT LOCATION REPORT. */
    TETRA_LIP_LONG_REPORT,      /**< LONG LOCATION REPORT. */
    TETRA_LIP_OTHER,            /**< Any other LIP PDU, see pdu_ext. */
};

/** Location shapes, table 6.79. */
enum tetra_lip_shape {
    TETRA_LIP_NO_SHAPE = 0,
    TETRA_LIP_POINT,
    TETRA_LIP_CIRCLE,
    TETRA_LIP_ELLIPSE,
    TETRA_LIP_POINT_ALT,
    TETRA_LIP_CIRCLE_ALT,
    TETRA_LIP_ELLIPSE_ALT,
    TETRA_LIP_CIRCLE_ALT_UNC,
    TETRA_LIP_ELLIPSE_ALT_UNC,
    TETRA_LIP_ARC,
    TETRA_LIP_POINT_ERROR,
};

/** Value of the code fields below when the element is absent. */
#define TETRA_LIP_NONE 0xff

/**
 * Decoded LIP PDU. Codes are the raw values of the tables in
 * TS 100 392-18-1; quantities with a unit are converted.
 */
struct tetra_lip {
    enum tetra_lip_kind kind;
    uint8_t pdu_ext; /**< PDU type extension (table 6.92), long PDUs. */

    /* Time: short reports and long reports with time type 1 give the
     * time elapsed, long reports with time type 2 the time of position. */
    uint8_t time_elapsed; /**< Table 6.111 code, TETRA_LIP_NONE if absent. */
    bool has_time_of_position;
    uint8_t day, hour, minute, second; /**< UTC. */

    /* Location */
    uint8_t shape; /**< enum tetra_lip_shape (long reports). */
    bool has_position;
    int32_t lat_e7;         /**< Latitude in 1e-7 degrees, north positive. */
    int32_t lon_e7;         /**< Longitude in 1e-7 degrees, east positive. */
    uint8_t position_error; /**< Table 6.93 code, TETRA_LIP_NONE. */
    uint8_t h_uncertainty;  /**< Table 6.49 code: circle radius or half
                                 major axis, TETRA_LIP_NONE. */
    uint8_t h_uncertainty_minor; /**< Ellipse: half minor axis code. */
    uint16_t angle_ddeg;  /**< Ellipse orientation, arc start (0.1 deg). */
    uint16_t angle2_ddeg; /**< Arc stop angle (0.1 deg). */
    uint32_t inner_radius_m, outer_radius_m; /**< Arc. */
    uint8_t confidence; /**< Table 6.43 code, TETRA_LIP_NONE. */
    bool has_altitude;
    bool altitude_user; /**< User defined reference, else WGS84. */
    int16_t altitude_m;
    uint8_t altitude_uncertainty; /**< Table 6.63 code, TETRA_LIP_NONE. */

    /* Velocity */
    uint16_t speed_dkmh;        /**< Horizontal speed in 0.1 km/h, 0xffff if not
                                     known, 0xfffe above 1043 km/h. */
    int16_t vspeed_dkmh;        /**< Vertical speed in 0.1 km/h, up positive,
                                     INT16_MIN if absent or not known. */
    uint16_t direction_ddeg;    /**< Direction of travel, 0.1 deg clockwise
                                     from north, 0xffff if not known. */
    uint8_t speed_uncertainty;  /**< Table 6.57 code, TETRA_LIP_NONE. */
    uint8_t vspeed_uncertainty; /**< Table 6.125 code, TETRA_LIP_NONE. */
    uint8_t direction_uncertainty; /**< Table 6.46 code, TETRA_LIP_NONE. */

    /* Additional data */
    bool ack_request;
    uint16_t reason;    /**< Reason for sending (table 6.94), 0xffff. */
    uint16_t user_data; /**< User defined data, 0xffff if absent. */

    /* Optional (type 5) elements of long reports */
    uint16_t msg_ref;     /**< Location message reference, 0xffff. */
    uint16_t result_code; /**< Table 6.98, 0xffff if absent. */
    bool has_status;
    uint16_t status; /**< Pre-coded status value. */
    bool has_sds1;
    uint16_t sds1;   /**< SDS type-1 value. */
    uint8_t id_type; /**< Terminal identification type (table 6.32),
                          TETRA_LIP_NONE if absent. */
    uint32_t id_ssi; /**< Terminal SSI, 0 if not given. */
    uint32_t id_mni; /**< Terminal MNI (MCC << 14 | MNC), 0. */
};

/**
 * \brief Recognise and decode a LIP message (protocol identifier 0x0a) in
 * an SDS event.
 *
 * @param sds: SDS event of type TETRA_SDS_UDD4.
 * @param lip: decoded result.
 * @return 0 if this is a LIP PDU, -1 otherwise.
 */
int tetra_sds_parse_lip(const struct tetra_sds *sds, struct tetra_lip *lip);

/**
 * \brief Horizontal position uncertainty (table 6.49) in metres.
 *
 * @param code: uncertainty code 0..62.
 * @return upper bound of the uncertainty in metres, 0 for code 63 (best
 * effort) or invalid codes.
 */
uint32_t tetra_lip_uncertainty_m(uint8_t code);

#ifdef __cplusplus
}
#endif

#endif /* TETRA_SDS_H */
