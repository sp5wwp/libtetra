/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * Host-side pi/4-DQPSK demodulator: complex baseband recording -> bits.
 *
 * Not part of the MCU library. It turns I/Q captures into the bit stream
 * that libtetra consumes, so recordings can be used for testing.
 *
 * Pipeline: frequency shift -> RRC matched filter evaluated at arbitrary
 * time instants -> Gardner timing recovery -> differential detection with
 * residual-rotation tracking -> soft or hard bits.
 */

#include <complex.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SYMRATE 18000.0
#define RRC_ALPHA 0.35
#define RRC_SPAN 4  /* symbols on each side */
#define RRC_RES 256 /* table points per symbol */

enum in_fmt { FMT_WAV, FMT_CS16, FMT_CF32, FMT_CU8 };
enum out_fmt { OUT_DIBIT, OUT_BIT, OUT_SOFT };

static float rrc_tab[2 * RRC_SPAN * RRC_RES + 1];

static double rrc(double t)
{
    const double a = RRC_ALPHA;
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

static void rrc_init(void)
{
    for (int i = 0; i <= 2 * RRC_SPAN * RRC_RES; i++)
        rrc_tab[i] = rrc((double)(i - RRC_SPAN * RRC_RES) / RRC_RES);
}

struct demod {
    double fs;
    double sps; /* input samples per symbol */
    double complex osc, osc_step;
    long osc_renorm;

    float complex *buf; /* mixed input samples */
    size_t buf_len, buf_cap;
    long buf_base; /* absolute index of buf[0] */

    double t_next; /* absolute sample time of next symbol */
    float complex prev_sym;
    double rot; /* residual rotation per symbol (rad) */
    double pwr_fast, pwr_slow, pwr_peak;
    double gain_ted, gain_rot;
    int have_prev;
    enum out_fmt out;
    FILE *fo;
    long nsym;
};

static float complex mf_at(struct demod *d, double t)
{
    long lo = (long)ceil(t - RRC_SPAN * d->sps);
    long hi = (long)floor(t + RRC_SPAN * d->sps);
    float complex acc = 0;

    for (long n = lo; n <= hi; n++) {
        long i = n - d->buf_base;
        double u = (t - n) / d->sps;
        long k;

        if (i < 0 || i >= (long)d->buf_len)
            continue;
        k = lround((u + RRC_SPAN) * RRC_RES);
        if (k < 0 || k > 2 * RRC_SPAN * RRC_RES)
            continue;
        acc += d->buf[i] * rrc_tab[k];
    }
    return acc;
}

static void emit(struct demod *d, float complex w)
{
    float m = cabsf(w) + 1e-12f;
    float sb1 = -cimagf(w) / m; /* B(2k-1) = 1 when Im < 0 */
    float sb2 = -crealf(w) / m; /* B(2k)   = 1 when Re < 0 */

    if (d->out == OUT_SOFT) {
        int8_t s[2];

        s[0] = (int8_t)lrintf(fmaxf(-127.f, fminf(127.f, sb1 * 127.f)));
        s[1] = (int8_t)lrintf(fmaxf(-127.f, fminf(127.f, sb2 * 127.f)));
        fwrite(s, 1, 2, d->fo);
    } else {
        uint8_t b1 = sb1 > 0, b2 = sb2 > 0;

        if (d->out == OUT_DIBIT) {
            uint8_t v = (uint8_t)((b1 << 1) | b2);

            fwrite(&v, 1, 1, d->fo);
        } else {
            fwrite(&b1, 1, 1, d->fo);
            fwrite(&b2, 1, 1, d->fo);
        }
    }
    d->nsym++;
}

/* Produce all symbols whose filter support is inside the buffer. */
static void run(struct demod *d, int flush)
{
    double margin = RRC_SPAN * d->sps + 2;
    double end = (double)(d->buf_base + (long)d->buf_len);

    while (d->t_next + margin < end || (flush && d->t_next < end)) {
        float complex y = mf_at(d, d->t_next);
        float complex ym = mf_at(d, d->t_next - d->sps / 2);
        double p = crealf(y * conjf(y));
        int active;

        d->pwr_fast = 0.9 * d->pwr_fast + 0.1 * p;
        d->pwr_slow = 0.999 * d->pwr_slow + 0.001 * p;
        if (d->pwr_slow > d->pwr_peak)
            d->pwr_peak = d->pwr_slow;
        else
            d->pwr_peak *= 0.99999;
        /* Freeze loops in the gaps between bursts. */
        active = d->pwr_fast > 0.05 * d->pwr_peak;

        if (d->have_prev) {
            float complex z = y * conjf(d->prev_sym);
            float complex w = z * cexpf(-I * (float)d->rot);

            if (active) {
                float complex w4 = w * w * w * w;
                double e = cargf(-w4) / 4.0;
                double ted = crealf((y - d->prev_sym) * conjf(ym)) /
                             (d->pwr_slow + 1e-9);

                d->rot += d->gain_rot * e;
                d->t_next -= d->gain_ted * ted * d->sps;
            }
            emit(d, w);
        }
        d->prev_sym = y;
        d->have_prev = 1;
        d->t_next += d->sps;
    }

    /* Drop samples no longer needed. */
    long keep_from = (long)floor(d->t_next - margin) - 1 - d->buf_base;

    if (keep_from > 0 && keep_from < (long)d->buf_len) {
        memmove(d->buf, d->buf + keep_from,
                (d->buf_len - keep_from) * sizeof(*d->buf));
        d->buf_len -= keep_from;
        d->buf_base += keep_from;
    }
}

static void push(struct demod *d, float complex s)
{
    if (d->buf_len == d->buf_cap) {
        d->buf_cap = d->buf_cap ? d->buf_cap * 2 : 65536;
        d->buf = realloc(d->buf, d->buf_cap * sizeof(*d->buf));
        if (!d->buf) {
            perror("realloc");
            exit(1);
        }
    }
    d->buf[d->buf_len++] = s * (float complex)d->osc;
    d->osc *= d->osc_step;
    if (++d->osc_renorm == 4096) {
        d->osc /= cabs(d->osc);
        d->osc_renorm = 0;
    }
}

static int wav_header(FILE *f, double *fs)
{
    uint8_t h[12], ck[8];
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;

    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) ||
        memcmp(h + 8, "WAVE", 4))
        return -1;
    while (fread(ck, 1, 8, f) == 8) {
        uint32_t len = ck[4] | ck[5] << 8 | ck[6] << 16 | (uint32_t)ck[7] << 24;

        if (!memcmp(ck, "fmt ", 4)) {
            uint8_t b[16];

            if (len < 16 || fread(b, 1, 16, f) != 16)
                return -1;
            fmt = b[0] | b[1] << 8;
            ch = b[2] | b[3] << 8;
            rate = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
            bits = b[14] | b[15] << 8;
            fseek(f, len - 16 + (len & 1), SEEK_CUR);
        } else if (!memcmp(ck, "data", 4)) {
            if (fmt != 1 || ch != 2 || bits != 16) {
                fprintf(stderr, "wav: need 16-bit stereo PCM\n");
                return -1;
            }
            *fs = rate;
            return 0;
        } else {
            fseek(f, len + (len & 1), SEEK_CUR);
        }
    }
    return -1;
}

static void usage(const char *p)
{
    fprintf(stderr,
            "usage: %s [-t wav|cs16|cf32|cu8] [-r rate] [-f offset_hz]\n"
            "          [-o dibit|bit|soft] [-q] input output\n"
            "  -f  carrier position relative to centre (e.g. -20000)\n"
            "  -o  dibit: one byte per symbol (B1<<1|B2), default\n"
            "      bit:   one byte per bit (0/1), osmo-tetra format\n"
            "      soft:  one int8 per bit, >0 means 1\n",
            p);
    exit(2);
}

int main(int argc, char **argv)
{
    enum in_fmt in = FMT_WAV;
    struct demod d;
    double off = 0;
    int opt, quiet = 0;
    FILE *fi;

    memset(&d, 0, sizeof(d));
    d.out = OUT_DIBIT;

    while ((opt = getopt(argc, argv, "t:r:f:o:q")) != -1) {
        switch (opt) {
        case 't':
            if (!strcmp(optarg, "wav"))
                in = FMT_WAV;
            else if (!strcmp(optarg, "cs16"))
                in = FMT_CS16;
            else if (!strcmp(optarg, "cf32"))
                in = FMT_CF32;
            else if (!strcmp(optarg, "cu8"))
                in = FMT_CU8;
            else
                usage(argv[0]);
            break;
        case 'r':
            d.fs = atof(optarg);
            break;
        case 'f':
            off = atof(optarg);
            break;
        case 'o':
            if (!strcmp(optarg, "dibit"))
                d.out = OUT_DIBIT;
            else if (!strcmp(optarg, "bit"))
                d.out = OUT_BIT;
            else if (!strcmp(optarg, "soft"))
                d.out = OUT_SOFT;
            else
                usage(argv[0]);
            break;
        case 'q':
            quiet = 1;
            break;
        default:
            usage(argv[0]);
        }
    }
    if (argc - optind != 2)
        usage(argv[0]);

    fi = fopen(argv[optind], "rb");
    d.fo = fopen(argv[optind + 1], "wb");
    if (!fi || !d.fo) {
        perror("open");
        return 1;
    }
    if (in == FMT_WAV && wav_header(fi, &d.fs)) {
        fprintf(stderr, "bad wav file\n");
        return 1;
    }
    if (d.fs <= 0) {
        fprintf(stderr, "sample rate required (-r)\n");
        return 1;
    }

    rrc_init();
    d.sps = d.fs / SYMRATE;
    d.osc = 1.0;
    d.osc_step = cexp(-I * 2.0 * M_PI * off / d.fs);
    d.t_next = RRC_SPAN * d.sps + 1;
    d.gain_ted = 0.02;
    d.gain_rot = 0.01;

    for (;;) {
        float complex s;

        if (in == FMT_WAV || in == FMT_CS16) {
            int16_t v[2];

            if (fread(v, sizeof(v), 1, fi) != 1)
                break;
            s = v[0] / 32768.f + I * (v[1] / 32768.f);
        } else if (in == FMT_CF32) {
            float v[2];

            if (fread(v, sizeof(v), 1, fi) != 1)
                break;
            s = v[0] + I * v[1];
        } else {
            uint8_t v[2];

            if (fread(v, sizeof(v), 1, fi) != 1)
                break;
            s = (v[0] - 127.5f) / 128.f + I * ((v[1] - 127.5f) / 128.f);
        }
        push(&d, s);
        if (d.buf_len >= 32768)
            run(&d, 0);
    }
    run(&d, 1);

    if (!quiet)
        fprintf(stderr, "%ld symbols, residual offset %.1f Hz\n", d.nsym,
                d.rot / (2 * M_PI) * SYMRATE);
    fclose(fi);
    fclose(d.fo);
    free(d.buf);
    return 0;
}
