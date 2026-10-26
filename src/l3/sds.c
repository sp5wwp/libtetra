/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * SDS-TL and text messaging, EN 300 392-2 clause 29.
 */

#include <string.h>
#include <tetra/tetra_sds.h>
#include "util/bits.h"

int tetra_sds_parse_text(const struct tetra_sds *sds, struct tetra_sds_text *t)
{
    struct bitrd b;
    unsigned pid;

    if (sds->type != TETRA_SDS_UDD4 || !sds->data || sds->nbits < 16)
        return -1;

    memset(t, 0, sizeof(*t));
    br_init(&b, sds->data, 0, sds->nbits);
    pid = br_get(&b, 8);
    t->protocol_id = (uint8_t)pid;

    switch (pid) {
    case TETRA_PID_SIMPLE_TEXT:
    case TETRA_PID_SIMPLE_IMM_TEXT:
        t->immediate = pid == TETRA_PID_SIMPLE_IMM_TEXT;
        br_skip(&b, 1);
        t->coding = (uint8_t)br_get(&b, 7);
        break;
    case TETRA_PID_TEXT:
    case TETRA_PID_IMM_TEXT: {
        unsigned fwd;

        t->sdstl = true;
        t->immediate = pid == TETRA_PID_IMM_TEXT;
        if (br_get(&b, 4) != 0)
            return -1; /* not SDS-TRANSFER */
        t->report_req = (uint8_t)br_get(&b, 2);
        br_skip(&b, 1);
        fwd = br_get(&b, 1); /* storage / forward control */
        t->msg_ref = (uint8_t)br_get(&b, 8);
        if (fwd) {
            unsigned type;

            br_skip(&b, 5); /* validity period */
            type = br_get(&b, 3);
            if (type == 0) {
                br_skip(&b, 8);
            } else if (type == 1) {
                br_skip(&b, 24);
            } else if (type == 2) {
                br_skip(&b, 48);
            } else if (type == 3) {
                unsigned digits = br_get(&b, 8);

                br_skip(&b, 4 * (digits + (digits & 1)));
            }
        }
        t->has_timestamp = br_get(&b, 1);
        t->coding = (uint8_t)br_get(&b, 7);
        if (t->has_timestamp)
            t->timestamp = br_get(&b, 24);
        break;
    }
    default:
        return -1;
    }
    if (b.err)
        return -1;
    t->data = sds->data;
    t->start = b.pos;
    t->nbits = (uint16_t)br_left(&b);
    return 0;
}

/* GSM 03.38 default alphabet as Unicode code points. */
static const uint16_t gsm7[128] = {
    0x0040, 0x00a3, 0x0024, 0x00a5, 0x00e8, 0x00e9, 0x00f9, 0x00ec, 0x00f2,
    0x00c7, 0x000a, 0x00d8, 0x00f8, 0x000d, 0x00c5, 0x00e5, 0x0394, 0x005f,
    0x03a6, 0x0393, 0x039b, 0x03a9, 0x03a0, 0x03a8, 0x03a3, 0x0398, 0x039e,
    0x00a0, 0x00c6, 0x00e6, 0x00df, 0x00c9, 0x0020, 0x0021, 0x0022, 0x0023,
    0x00a4, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002a, 0x002b, 0x002c,
    0x002d, 0x002e, 0x002f, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035,
    0x0036, 0x0037, 0x0038, 0x0039, 0x003a, 0x003b, 0x003c, 0x003d, 0x003e,
    0x003f, 0x00a1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004a, 0x004b, 0x004c, 0x004d, 0x004e, 0x004f, 0x0050,
    0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059,
    0x005a, 0x00c4, 0x00d6, 0x00d1, 0x00dc, 0x00a7, 0x00bf, 0x0061, 0x0062,
    0x0063, 0x0064, 0x0065, 0x0066, 0x0067, 0x0068, 0x0069, 0x006a, 0x006b,
    0x006c, 0x006d, 0x006e, 0x006f, 0x0070, 0x0071, 0x0072, 0x0073, 0x0074,
    0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007a, 0x00e4, 0x00f6, 0x00f1,
    0x00fc, 0x00e0,
};

/* GSM 03.38 extension table (after the 0x1b escape). */
static uint16_t gsm7_ext(unsigned c)
{
    switch (c) {
    case 0x0a:
        return 0x000c;
    case 0x14:
        return 0x005e;
    case 0x28:
        return 0x007b;
    case 0x29:
        return 0x007d;
    case 0x2f:
        return 0x005c;
    case 0x3c:
        return 0x005b;
    case 0x3d:
        return 0x007e;
    case 0x3e:
        return 0x005d;
    case 0x40:
        return 0x007c;
    case 0x65:
        return 0x20ac;
    default:
        return 0x003f;
    }
}

struct utf8w {
    char *p;
    size_t left;
    int n;
};

static void put_cp(struct utf8w *w, uint32_t cp)
{
    char tmp[4];
    size_t k;

    if (cp < 0x80) {
        tmp[0] = (char)cp;
        k = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xc0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3f));
        k = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xe0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        tmp[2] = (char)(0x80 | (cp & 0x3f));
        k = 3;
    } else {
        tmp[0] = (char)(0xf0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        tmp[3] = (char)(0x80 | (cp & 0x3f));
        k = 4;
    }
    if (k + 1 > w->left)
        return;
    memcpy(w->p, tmp, k);
    w->p += k;
    w->left -= k;
    w->n += (int)k;
}

static unsigned octet_at(const struct tetra_sds_text *t, unsigned i)
{
    unsigned v = 0;

    for (unsigned k = 0; k < 8; k++)
        v = (v << 1) | bit_at(t->data, t->start + 8 * i + k);
    return v;
}

static void decode_gsm7(const struct tetra_sds_text *t, struct utf8w *w)
{
    unsigned noct = t->nbits / 8;
    unsigned nchar = noct * 8 / 7;
    bool esc = false;

    for (unsigned i = 0; i < nchar; i++) {
        /* GSM packing: septet i starts at bit 7*i, LSB first in octets */
        unsigned bit = 7 * i;
        unsigned o = bit / 8, sh = bit % 8;
        unsigned v = octet_at(t, o) >> sh;

        if (sh > 1 && o + 1 < noct)
            v |= octet_at(t, o + 1) << (8 - sh);
        v &= 0x7f;

        /* Seven padding zeros at the end would read as '@'. */
        if (i == nchar - 1 && v == 0 && (noct * 8) % 7 == 0)
            break;
        if (esc) {
            put_cp(w, gsm7_ext(v));
            esc = false;
        } else if (v == 0x1b) {
            esc = true;
        } else {
            put_cp(w, gsm7[v]);
        }
    }
}

static void decode_ucs2(const struct tetra_sds_text *t, struct utf8w *w)
{
    unsigned n = t->nbits / 16;

    for (unsigned i = 0; i < n; i++) {
        uint32_t c = octet_at(t, 2 * i) << 8 | octet_at(t, 2 * i + 1);

        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n) {
            uint32_t lo = octet_at(t, 2 * i + 2) << 8 | octet_at(t, 2 * i + 3);

            if (lo >= 0xdc00 && lo < 0xe000) {
                c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                i++;
            }
        }
        put_cp(w, c);
    }
}

int tetra_sds_text_utf8(const struct tetra_sds_text *t, char *out, size_t len)
{
    struct utf8w w = { out, len, 0 };

    if (!len)
        return -1;

    if (t->coding == TETRA_TCS_GSM7) {
        decode_gsm7(t, &w);
    } else if (t->coding == TETRA_TCS_UCS2) {
        decode_ucs2(t, &w);
    } else {
        for (unsigned i = 0; i < t->nbits / 8u; i++) {
            unsigned c = octet_at(t, i);

            if (t->coding == TETRA_TCS_LATIN1 || c < 0x80)
                put_cp(&w, c);
            else
                put_cp(&w, '?');
        }
    }
    *w.p = '\0';
    return w.n;
}
