/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * Synthetic TMO downlink with continuous speech traffic, for receiver tests:
 * a cell (sync bursts and SYSINFO in frame 18) with clear speech on one
 * timeslot in frames 1 to 17 of every multiframe, the other slots carrying
 * null PDUs. The speech frames come from a file in ETSI serial format, as
 * tetra_rx -s writes it (frames with the BFI set are skipped), looped; or
 * are pseudo-random without one.
 *
 * The output is a whole number of multiframes, modulated circularly
 * (pi/4-DQPSK, RRC 0.35), so it can be replayed in a loop without any jump
 * in the signal: the TDMA counters run on, the RRC filter wraps around, and
 * the phase left over at the end is spread over the file as a frequency
 * offset of a few mHz.
 *
 *   tetra_tmo_gen [-n multiframes] [-t tn] [-c mcc:mnc:cc] [-f dl_hz]
 *                 [-r rate] [-s speech.ser] [-b bits.out] out.cs16
 *
 * Output: interleaved int16 I/Q (UHD sc16), peak at half of full scale.
 */

#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tetra/tetra.h>
#include <tetra/tetra_coding.h>
#include <tetra/tetra_tx.h>

#define SYMRATE 18000L
#define SLOT_SYMBOLS 255
#define SPAN 8 /* RRC taps on each side, in symbols */

struct cfg {
    uint16_t mcc, mnc;
    uint8_t cc;
    uint8_t tch_tn;
    uint8_t usage;
    uint16_t carrier; /* SYSINFO main carrier, band and offset */
    uint8_t band, offset;
};

/* Speech frames, 137 bits each, one per byte */
static uint8_t (*frames)[TETRA_SPEECH_BITS];
static size_t n_frames, next_frame;
static uint32_t rng = 0x2545f491;

static uint32_t xorshift(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void next_speech(uint8_t out[TETRA_SPEECH_BITS])
{
    if (n_frames == 0) {
        for (unsigned i = 0; i < TETRA_SPEECH_BITS; i++)
            out[i] = xorshift() & 1;
        return;
    }
    memcpy(out, frames[next_frame], TETRA_SPEECH_BITS);
    next_frame = (next_frame + 1) % n_frames;
}

static void load_speech(const char *path)
{
    FILE *f = fopen(path, "rb");
    int16_t w[1 + TETRA_SPEECH_BITS];
    size_t cap = 0, skipped = 0;

    if (!f) {
        perror(path);
        exit(1);
    }
    while (fread(w, sizeof(w[0]), 1 + TETRA_SPEECH_BITS, f) ==
           1 + TETRA_SPEECH_BITS) {
        if (w[0] != 0) {
            skipped++;
            continue;
        }
        if (n_frames == cap) {
            cap = cap ? 2 * cap : 256;
            frames = realloc(frames, cap * sizeof(*frames));
            if (!frames)
                exit(1);
        }
        for (unsigned i = 0; i < TETRA_SPEECH_BITS; i++)
            frames[n_frames][i] = w[1 + i] != 0;
        n_frames++;
    }
    fclose(f);
    if (n_frames == 0) {
        fprintf(stderr, "%s: no good speech frames\n", path);
        exit(1);
    }
    fprintf(stderr, "%zu speech frames (%zu bad ones skipped)\n", n_frames,
            skipped);
}

/* MSB-first bit writer into 0/1 bytes */
static void put(uint8_t *bits, unsigned *pos, uint32_t v, unsigned n)
{
    while (n--)
        bits[(*pos)++] = (v >> n) & 1;
}

static void null_block(uint8_t t1[124])
{
    unsigned pos = 0;

    memset(t1, 0, 124);
    put(t1, &pos, 0, 2); /* MAC-RESOURCE */
    put(t1, &pos, 1, 1); /* fill bits */
    put(t1, &pos, 0, 4);
    put(t1, &pos, 2, 6); /* length: 2 octets */
    put(t1, &pos, 0, 3); /* null PDU */
    put(t1, &pos, 1, 1); /* fill bit */
}

/* One TMO downlink slot (510 bits) */
static void gen_slot(const struct cfg *c, uint8_t tn, uint8_t fn, uint8_t mn,
                     uint8_t slot[510])
{
    const uint32_t sc =
        tetra_scramb_init(tetra_tmo_colour_code(c->mcc, c->mnc, c->cc));
    const bool tch = c->tch_tn == tn && fn != 18;
    uint8_t bb[30], b1[216], b2[216], t5[432], t1[124];
    uint16_t aach = 0; /* common control */

    if (tch)
        aach = (uint16_t)((1u << 12) | (c->usage << 6)); /* DL traffic */
    tetra_encode_aach(aach, sc, bb);

    if (fn == 18 && (mn + tn) % 4 == 3) {
        /* Synchronisation burst: SYNC on the BSCH, SYSINFO in block 2 */
        uint8_t s[60], sb[120];
        unsigned pos = 0;

        put(s, &pos, 1, 4); /* system code */
        put(s, &pos, c->cc, 6);
        put(s, &pos, tn - 1u, 2);
        put(s, &pos, fn, 5);
        put(s, &pos, mn, 6);
        put(s, &pos, 0, 2 + 3 + 1 + 1 + 1);
        put(s, &pos, c->mcc, 10);
        put(s, &pos, c->mnc, 14);
        put(s, &pos, 0, 5);
        tetra_encode_sch(TETRA_SCH_S, s, tetra_scramb_init(0), sb);

        memset(t1, 0, sizeof(t1));
        pos = 0;
        put(t1, &pos, 2, 2); /* MAC PDU type: broadcast */
        put(t1, &pos, 0, 2); /* SYSINFO */
        put(t1, &pos, c->carrier, 12);
        put(t1, &pos, c->band, 4);
        put(t1, &pos, c->offset, 2);
        put(t1, &pos, 0, 3 + 1 + 2 + 3 + 4 + 4 + 4);
        put(t1, &pos, 0, 1);  /* no CCK */
        put(t1, &pos, 0, 16); /* hyperframe */
        put(t1, &pos, 2, 2);
        put(t1, &pos, 0, 20);
        put(t1, &pos, 1, 14); /* location area */
        put(t1, &pos, 0xffff, 16);
        put(t1, &pos, 0xd70, 12);
        tetra_encode_sch(TETRA_SCH_H, t1, sc, b2);
        tetra_tmo_build_sb(slot, sb, bb, b2);
    } else if (tch) {
        uint8_t a[TETRA_SPEECH_BITS], b[TETRA_SPEECH_BITS];

        next_speech(a);
        next_speech(b);
        tetra_speech_encode_slot(a, b, t5);
        tetra_scramble(t5, 432, sc);
        tetra_tmo_build_ndb(slot, t5, bb, t5 + 216, false);
    } else {
        null_block(t1);
        tetra_encode_sch(TETRA_SCH_H, t1, sc, b1);
        tetra_encode_sch(TETRA_SCH_H, t1, sc, b2);
        tetra_tmo_build_ndb(slot, b1, bb, b2, true);
    }
}

static double rrc(double t)
{
    const double a = 0.35;
    double x;

    if (fabs(t) < 1e-9)
        return 1.0 - a + 4.0 * a / M_PI;
    if (fabs(fabs(t) - 1.0 / (4.0 * a)) < 1e-9) {
        x = (1.0 + 2.0 / M_PI) * sin(M_PI / (4.0 * a)) +
            (1.0 - 2.0 / M_PI) * cos(M_PI / (4.0 * a));
        return a / sqrt(2.0) * x;
    }
    x = sin(M_PI * t * (1.0 - a)) + 4.0 * a * t * cos(M_PI * t * (1.0 + a));
    return x / (M_PI * t * (1.0 - 16.0 * a * a * t * t));
}

static void usage(const char *name)
{
    fprintf(stderr,
            "usage: %s [-n multiframes] [-t tn] [-c mcc:mnc:cc] [-f dl_hz]\n"
            "          [-r rate] [-s speech.ser] [-b bits.out] out.cs16\n"
            "  -n  multiframes (default 60: the counters wrap, 61.2 s)\n"
            "  -t  traffic timeslot 1..4 (default 2)\n"
            "  -c  cell (default 901:9999:1)\n"
            "  -f  downlink frequency for SYSINFO, 400 MHz band "
            "(default 438275000)\n"
            "  -r  output sample rate (default 250000)\n"
            "  -s  speech frames, ETSI serial format (tetra_rx -s)\n"
            "  -b  also write the bits, one per byte (tetra_rx -t bit)\n",
            name);
    exit(2);
}

int main(int argc, char **argv)
{
    struct cfg c = {
        .mcc = 901, .mnc = 9999, .cc = 1, .tch_tn = 2, .usage = 4, .band = 4
    };
    long mf = 60, rate = 250000;
    double dl = 438275000.0;
    const char *bits_path = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "n:t:c:f:r:s:b:")) != -1) {
        switch (opt) {
        case 'n':
            mf = atol(optarg);
            break;
        case 't':
            c.tch_tn = (uint8_t)atoi(optarg);
            break;
        case 'c': {
            unsigned a, b, d;

            if (sscanf(optarg, "%u:%u:%u", &a, &b, &d) != 3)
                usage(argv[0]);
            c.mcc = (uint16_t)a;
            c.mnc = (uint16_t)b;
            c.cc = (uint8_t)d;
            break;
        }
        case 'f':
            dl = atof(optarg);
            break;
        case 'r':
            rate = atol(optarg);
            break;
        case 's':
            load_speech(optarg);
            break;
        case 'b':
            bits_path = optarg;
            break;
        default:
            usage(argv[0]);
        }
    }
    if (optind + 1 != argc || mf < 1 || c.tch_tn < 1 || c.tch_tn > 4)
        usage(argv[0]);

    /* SYSINFO: carrier number in 25 kHz steps from the band base */
    double rel = dl - 400e6;
    long carrier = lround(rel / 25e3);
    if (rel < 0 || carrier > 4095 || fabs(rel - carrier * 25e3) > 1) {
        fprintf(stderr, "-f: 400-500 MHz on a 25 kHz raster only\n");
        return 2;
    }
    c.carrier = (uint16_t)carrier;

    /* A whole number of output samples per multiframe */
    const long mf_sym = 18L * 4 * SLOT_SYMBOLS;
    if ((mf_sym * rate) % SYMRATE) {
        fprintf(stderr,
                "-r: %ld S/s gives no whole number of samples per "
                "multiframe\n",
                rate);
        return 2;
    }

    /* Bits, then phases */
    const long n_sym = mf * mf_sym;
    uint8_t *bits = malloc((size_t)n_sym * 2);
    double complex *sym = malloc((size_t)n_sym * sizeof(*sym));
    if (!bits || !sym)
        return 1;
    long k = 0;
    for (long m = 0; m < mf; m++)
        for (uint8_t fn = 1; fn <= 18; fn++)
            for (uint8_t tn = 1; tn <= 4; tn++, k++)
                gen_slot(&c, tn, fn, (uint8_t)(m % 60 + 1), bits + k * 510);
    if (bits_path) {
        FILE *f = fopen(bits_path, "wb");
        if (!f || fwrite(bits, 1, (size_t)n_sym * 2, f) != (size_t)n_sym * 2) {
            perror(bits_path);
            return 1;
        }
        fclose(f);
    }

    static const int dphi[4] = { 1, 3, -1, -3 }; /* units of pi/4 */
    double phase = 0;
    for (long i = 0; i < n_sym; i++) {
        phase += M_PI / 4 * dphi[bits[2 * i] << 1 | bits[2 * i + 1]];
        phase = remainder(phase, 2 * M_PI);
        sym[i] = phase;
    }
    /*
     * Make the loop seamless: symbol 0 of the next loop must follow the last
     * symbol by its own phase step. Spread the error over the file.
     */
    double err = remainder(sym[n_sym - 1] +
                               M_PI / 4 * dphi[bits[0] << 1 | bits[1]] - sym[0],
                           2 * M_PI);
    for (long i = 0; i < n_sym; i++)
        sym[i] = cexp(I * (creal(sym[i]) - err * i / n_sym));
    fprintf(stderr, "loop phase correction %.3f rad (%.4f Hz)\n", err,
            err / (2 * M_PI) * SYMRATE / n_sym);

    /* Circular RRC modulation */
    const long n_out = n_sym * rate / SYMRATE;
    float complex *out = malloc((size_t)n_out * sizeof(*out));
    if (!out)
        return 1;
    double peak = 0;
    for (long n = 0; n < n_out; n++) {
        double t = (double)n * SYMRATE / rate;
        long k0 = (long)floor(t) - SPAN;
        double complex acc = 0;

        for (long j = k0; j <= k0 + 2 * SPAN + 1; j++)
            acc += sym[((j % n_sym) + n_sym) % n_sym] * rrc(t - (double)j);
        out[n] = (float complex)acc;
        if (cabs(acc) > peak)
            peak = cabs(acc);
    }

    FILE *f = fopen(argv[optind], "wb");
    if (!f) {
        perror(argv[optind]);
        return 1;
    }
    const double scale = 16384.0 / peak;
    for (long n = 0; n < n_out; n++) {
        int16_t v[2] = { (int16_t)lrint(crealf(out[n]) * scale),
                         (int16_t)lrint(cimagf(out[n]) * scale) };

        fwrite(v, sizeof(v), 1, f);
    }
    fclose(f);
    fprintf(stderr, "%ld multiframes, %.1f s at %ld S/s, speech on TN%u\n", mf,
            (double)n_sym / SYMRATE, rate, c.tch_tn);
    return 0;
}
