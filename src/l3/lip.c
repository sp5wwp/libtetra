/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * Location Information Protocol location reports, ETSI TS 100 392-18-1
 * clauses 6.2 to 6.4. Integer arithmetic only.
 */

#include <string.h>
#include <tetra/tetra_sds.h>
#include "util/bits.h"

/* LIP PDU type extension of the LONG LOCATION REPORT, table 6.92. */
#define LIP_EXT_LONG_REPORT 3

/* Type 5 element identifiers, table 6.119. */
enum {
    T5_LOCATION_MESSAGE_REFERENCE = 6,
    T5_RESULT_CODE = 12,
    T5_SDS_TYPE1_VALUE = 13,
    T5_STATUS_VALUE = 15,
    T5_TERMINAL_ID = 17,
    T5_EXTENDED = 31,
};

static int32_t sign_extend(uint32_t v, unsigned bits)
{
    uint32_t m = 1u << (bits - 1);

    return (int32_t)((v ^ m) - m);
}

/*
 * Latitude (24 bits, 180 / 2^24 degrees) and longitude (25 bits,
 * 360 / 2^25 degrees) share the same step: convert to 1e-7 degrees.
 */
static int32_t coord_e7(int32_t raw)
{
    const int64_t d = (int64_t)1 << 24;
    int64_t v = (int64_t)raw * 1800000000;

    return (int32_t)(v >= 0 ? (v + d / 2) / d : -((-v + d / 2) / d));
}

/* x * (num / den)^n in Q20 fixed point. */
static uint64_t pow_q20(uint64_t x, unsigned num, unsigned den, unsigned n)
{
    while (n--)
        x = (x * num + den / 2) / den;
    return x;
}

/* Horizontal or vertical speed code (table 6.51) to 0.1 km/h. */
static uint16_t speed_dkmh(unsigned k)
{
    if (k <= 28)
        return (uint16_t)(k * 10);
    if (k == 126)
        return 0xfffe;
    if (k == 127)
        return 0xffff;
    /* v = 16 * 1.038^(K - 13) km/h */
    return (uint16_t)((pow_q20(16ull << 20, 1038, 1000, k - 13) * 10 +
                       (1u << 19)) >>
                      20);
}

uint32_t tetra_lip_uncertainty_m(uint8_t code)
{
    if (code >= 63)
        return 0;
    /* 2 * 1.2^(K + 5) - 4 metres */
    return (uint32_t)((pow_q20(2ull << 20, 12, 10, code + 5u) + (1u << 19)) >>
                      20) -
           4;
}

/* Location altitude, table 6.62 (12 bits including the type). */
static void altitude(struct tetra_lip *l, struct bitrd *b)
{
    unsigned a;

    l->altitude_user = br_get(b, 1);
    a = br_get(b, 11);
    l->has_altitude = a != 0;
    if (a <= 1201)
        l->altitude_m = (int16_t)((int)a - 201);
    else if (a <= 1926)
        l->altitude_m = (int16_t)(1000 + (int)(a - 1201) * 2);
    else
        l->altitude_m = (int16_t)(2450 + (int)(a - 1926) * 75);
}

static uint16_t angle_ddeg(unsigned k)
{
    return (uint16_t)((k * 3600 + 128) / 256);
}

static void position(struct tetra_lip *l, struct bitrd *b)
{
    l->lon_e7 = coord_e7(sign_extend(br_get(b, 25), 25));
    l->lat_e7 = coord_e7(sign_extend(br_get(b, 24), 24));
    l->has_position = true;
}

static void ellipse_axes(struct tetra_lip *l, struct bitrd *b)
{
    l->h_uncertainty = (uint8_t)br_get(b, 6);
    l->h_uncertainty_minor = (uint8_t)br_get(b, 6);
    l->angle_ddeg = angle_ddeg(br_get(b, 8));
}

/* Location data, table 6.69. Returns false if the shape is not known. */
static bool location_data(struct tetra_lip *l, struct bitrd *b)
{
    l->shape = (uint8_t)br_get(b, 4);

    switch (l->shape) {
    case TETRA_LIP_NO_SHAPE:
        break;
    case TETRA_LIP_POINT:
        position(l, b);
        break;
    case TETRA_LIP_CIRCLE:
        position(l, b);
        l->h_uncertainty = (uint8_t)br_get(b, 6);
        break;
    case TETRA_LIP_ELLIPSE:
        position(l, b);
        ellipse_axes(l, b);
        l->confidence = (uint8_t)br_get(b, 3);
        break;
    case TETRA_LIP_POINT_ALT:
        position(l, b);
        altitude(l, b);
        break;
    case TETRA_LIP_CIRCLE_ALT:
    case TETRA_LIP_CIRCLE_ALT_UNC:
        position(l, b);
        l->h_uncertainty = (uint8_t)br_get(b, 6);
        altitude(l, b);
        if (l->shape == TETRA_LIP_CIRCLE_ALT_UNC)
            l->altitude_uncertainty = (uint8_t)br_get(b, 3);
        break;
    case TETRA_LIP_ELLIPSE_ALT:
    case TETRA_LIP_ELLIPSE_ALT_UNC:
        position(l, b);
        ellipse_axes(l, b);
        altitude(l, b);
        if (l->shape == TETRA_LIP_ELLIPSE_ALT_UNC)
            l->altitude_uncertainty = (uint8_t)br_get(b, 3);
        l->confidence = (uint8_t)br_get(b, 3);
        break;
    case TETRA_LIP_ARC:
        position(l, b);
        l->inner_radius_m = br_get(b, 16) * 2;
        l->outer_radius_m = br_get(b, 16) * 2;
        l->angle_ddeg = angle_ddeg(br_get(b, 8));
        l->angle2_ddeg = angle_ddeg(br_get(b, 8));
        l->confidence = (uint8_t)br_get(b, 3);
        break;
    case TETRA_LIP_POINT_ERROR:
        position(l, b);
        l->position_error = (uint8_t)br_get(b, 3);
        break;
    default:
        return false; /* reserved or shape extension: length unknown */
    }
    return true;
}

static void vertical_speed(struct tetra_lip *l, struct bitrd *b)
{
    unsigned down = br_get(b, 1), k = br_get(b, 7);
    uint16_t v = speed_dkmh(k);

    if (v >= 0xfffe)
        l->vspeed_dkmh = v == 0xfffe ? (down ? -10430 : 10430) : INT16_MIN;
    else
        l->vspeed_dkmh = (int16_t)(down ? -(int)v : (int)v);
}

/* Velocity data, table 6.122. */
static void velocity_data(struct tetra_lip *l, struct bitrd *b)
{
    unsigned type = br_get(b, 3);
    bool unc = type == 2 || type == 4 || type == 6 || type == 7;
    bool vert = type == 3 || type == 4 || type == 7;
    bool dir = type >= 5;

    if (type == 0)
        return;
    l->speed_dkmh = speed_dkmh(br_get(b, 7));
    if (unc)
        l->speed_uncertainty = (uint8_t)br_get(b, 3);
    if (vert) {
        vertical_speed(l, b);
        if (unc)
            l->vspeed_uncertainty = (uint8_t)br_get(b, 3);
    }
    if (dir) {
        l->direction_ddeg = angle_ddeg(br_get(b, 8));
        if (unc)
            l->direction_uncertainty = (uint8_t)br_get(b, 3);
    }
}

/* Reason for sending or user defined data, tables 6.120 and 6.94. */
static void additional_data(struct tetra_lip *l, struct bitrd *b)
{
    if (br_get(b, 1))
        l->user_data = (uint16_t)br_get(b, 8);
    else
        l->reason = (uint16_t)br_get(b, 8);
}

/* Terminal or location identification, table 6.110 (SSI part only). */
static void terminal_id(struct tetra_lip *l, struct bitrd *b)
{
    l->id_type = (uint8_t)br_get(b, 4);
    switch (l->id_type) {
    case 1: /* SSI */
    case 9: /* SSI and external subscriber number */
        l->id_ssi = br_get(b, 24);
        break;
    case 2:  /* SSI and MNI */
    case 10: /* SSI, MNI and external subscriber number */
        l->id_ssi = br_get(b, 24);
        l->id_mni = br_get(b, 24);
        break;
    default:
        break;
    }
}

/* Type 5 elements, clause 6.4: until the end of the PDU. */
static void type5_elements(struct tetra_lip *l, struct bitrd *b)
{
    while (br_left(b) >= 11) {
        unsigned id = br_get(b, 5), len = br_get(b, 6);
        struct bitrd e;

        if (len == 0)
            len = (br_get(b, 7) + 7) * 8;
        if (id == T5_EXTENDED || b->err || len > br_left(b))
            return;
        br_init(&e, b->buf, b->pos, b->pos + len);
        br_skip(b, len);

        switch (id) {
        case T5_LOCATION_MESSAGE_REFERENCE:
            l->msg_ref = (uint16_t)br_get(&e, 8);
            break;
        case T5_RESULT_CODE:
            l->result_code = (uint16_t)br_get(&e, 8);
            break;
        case T5_SDS_TYPE1_VALUE:
            l->sds1 = (uint16_t)br_get(&e, 16);
            l->has_sds1 = !e.err;
            break;
        case T5_STATUS_VALUE:
            l->status = (uint16_t)br_get(&e, 16);
            l->has_status = !e.err;
            break;
        case T5_TERMINAL_ID:
            terminal_id(l, &e);
            break;
        default:
            break;
        }
    }
}

int tetra_sds_parse_lip(const struct tetra_sds *sds, struct tetra_lip *l)
{
    struct bitrd b;
    unsigned type;

    if (sds->type != TETRA_SDS_UDD4 || !sds->data || sds->nbits < 10)
        return -1;

    memset(l, 0, sizeof(*l));
    l->time_elapsed = l->position_error = l->h_uncertainty =
        l->h_uncertainty_minor = l->confidence = l->altitude_uncertainty =
            l->speed_uncertainty = l->vspeed_uncertainty =
                l->direction_uncertainty = l->id_type = TETRA_LIP_NONE;
    l->speed_dkmh = l->direction_ddeg = 0xffff;
    l->reason = l->user_data = l->msg_ref = l->result_code = 0xffff;
    l->vspeed_dkmh = INT16_MIN;

    br_init(&b, sds->data, 0, sds->nbits);
    if (br_get(&b, 8) != TETRA_PID_LIP)
        return -1;
    type = br_get(&b, 2);

    if (type == 0) {
        /* SHORT LOCATION REPORT, table 6.1 */
        l->kind = TETRA_LIP_SHORT_REPORT;
        l->time_elapsed = (uint8_t)br_get(&b, 2);
        position(l, &b);
        l->shape = TETRA_LIP_POINT_ERROR;
        l->position_error = (uint8_t)br_get(&b, 3);
        l->speed_dkmh = speed_dkmh(br_get(&b, 7));
        l->direction_ddeg = (uint16_t)(br_get(&b, 4) * 225);
        additional_data(l, &b);
        return b.err ? -1 : 0;
    }
    if (type != 1)
        return -1;

    l->pdu_ext = (uint8_t)br_get(&b, 4);
    if (l->pdu_ext != LIP_EXT_LONG_REPORT) {
        l->kind = TETRA_LIP_OTHER;
        return b.err ? -1 : 0;
    }

    /* LONG LOCATION REPORT, table 6.2 */
    l->kind = TETRA_LIP_LONG_REPORT;
    switch (br_get(&b, 2)) { /* time type, table 6.114 */
    case 1:
        l->time_elapsed = (uint8_t)br_get(&b, 2);
        break;
    case 2:
        l->has_time_of_position = true;
        l->day = (uint8_t)br_get(&b, 5);
        l->hour = (uint8_t)br_get(&b, 5);
        l->minute = (uint8_t)br_get(&b, 6);
        l->second = (uint8_t)br_get(&b, 6);
        break;
    default:
        break;
    }
    if (!location_data(l, &b))
        return b.err ? -1 : 0; /* rest cannot be located */
    velocity_data(l, &b);
    l->ack_request = br_get(&b, 1);
    additional_data(l, &b);
    if (b.err)
        return -1;
    type5_elements(l, &b);
    return 0;
}
