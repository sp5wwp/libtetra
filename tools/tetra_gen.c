/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * Synthetic DMO signal generator. Writes the reference scenario either as
 * a bit stream or as modulated complex baseband (pi/4-DQPSK, RRC 0.35)
 * with silence between bursts, a frequency offset and noise.
 */

#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dmo_scenario.h"

#define SYMRATE 18000.0
#define SPAN 4

enum out_fmt { OUT_DIBIT, OUT_BIT, OUT_CF32, OUT_CS16 };

struct gen {
    enum out_fmt fmt;
    FILE *f;
    double fs, off, snr_db;
    /* modulator */
    double complex phase;
    double t_sym; /* symbol index of the next symbol */
    long n_out;   /* samples written */
    double complex *sym;
    double *amp;
    size_t nsym, cap;
    uint32_t rng;
};

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

static double gauss(struct gen *g)
{
    double u1, u2;

    g->rng = g->rng * 1103515245u + 12345u;
    u1 = ((g->rng >> 8) + 1.0) / 16777217.0;
    g->rng = g->rng * 1103515245u + 12345u;
    u2 = ((g->rng >> 8) + 1.0) / 16777217.0;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static void add_symbol(struct gen *g, double complex s, double a)
{
    if (g->nsym == g->cap) {
        g->cap = g->cap ? 2 * g->cap : 1 << 16;
        g->sym = realloc(g->sym, g->cap * sizeof(*g->sym));
        g->amp = realloc(g->amp, g->cap * sizeof(*g->amp));
        if (!g->sym || !g->amp)
            exit(1);
    }
    g->sym[g->nsym] = s;
    g->amp[g->nsym] = a;
    g->nsym++;
}

/* Render all samples whose filter support lies within known symbols. */
static void render(struct gen *g, int flush)
{
    double sps = g->fs / SYMRATE;
    double sigma = pow(10.0, -g->snr_db / 20.0) / sqrt(2.0);

    for (;;) {
        double t = g->n_out / sps; /* in symbols */
        long k0 = (long)floor(t) - SPAN, k1 = (long)ceil(t) + SPAN;
        double complex acc = 0;

        if (!flush && k1 >= (long)g->nsym)
            break;
        if (flush && t > (double)g->nsym)
            break;
        for (long k = k0; k <= k1; k++) {
            if (k < 0 || k >= (long)g->nsym)
                continue;
            acc += g->sym[k] * g->amp[k] * rrc(t - (double)k);
        }
        acc *= cexp(I * 2.0 * M_PI * g->off * g->n_out / g->fs);
        acc += sigma * (gauss(g) + I * gauss(g));
        if (g->fmt == OUT_CF32) {
            float v[2] = { (float)creal(acc) * 0.5f, (float)cimag(acc) * 0.5f };

            fwrite(v, sizeof(v), 1, g->f);
        } else {
            int16_t v[2] = { (int16_t)lrint(creal(acc) * 12000.0),
                             (int16_t)lrint(cimag(acc) * 12000.0) };

            fwrite(v, sizeof(v), 1, g->f);
        }
        g->n_out++;
    }
}

static void on_slot(void *user, const uint8_t bits[510], bool active)
{
    static const int dphi[4] = { 1, 3, -1, -3 };
    struct gen *g = user;

    if (g->fmt == OUT_BIT) {
        fwrite(bits, 1, 510, g->f);
        return;
    }
    if (g->fmt == OUT_DIBIT) {
        for (unsigned i = 0; i < 510; i += 2) {
            uint8_t d = (uint8_t)(bits[i] << 1 | bits[i + 1]);

            fwrite(&d, 1, 1, g->f);
        }
        return;
    }
    for (unsigned n = 0; n < 255; n++) {
        /* symbols 17..252 (1-based) carry the burst and its reference */
        double a = (active && n >= 16 && n <= 251) ? 1.0 : 0.0;
        int d = dphi[bits[2 * n] << 1 | bits[2 * n + 1]];

        g->phase *= cexp(I * M_PI / 4.0 * d);
        add_symbol(g, g->phase, a);
    }
    render(g, 0);
}

static void usage(const char *p)
{
    fprintf(stderr,
            "usage: %s [-o dibit|bit|cf32|cs16] [-r rate] [-f offset_hz]\n"
            "          [-s snr_db] output\n",
            p);
    exit(2);
}

int main(int argc, char **argv)
{
    struct gen g;
    struct dmo_scn s;
    unsigned ntraffic;
    int opt;

    memset(&g, 0, sizeof(g));
    g.fmt = OUT_DIBIT;
    g.fs = 250000;
    g.snr_db = 30;
    g.phase = 1;
    g.rng = 12345;

    while ((opt = getopt(argc, argv, "o:r:f:s:")) != -1) {
        switch (opt) {
        case 'o':
            if (!strcmp(optarg, "dibit"))
                g.fmt = OUT_DIBIT;
            else if (!strcmp(optarg, "bit"))
                g.fmt = OUT_BIT;
            else if (!strcmp(optarg, "cf32"))
                g.fmt = OUT_CF32;
            else if (!strcmp(optarg, "cs16"))
                g.fmt = OUT_CS16;
            else
                usage(argv[0]);
            break;
        case 'r':
            g.fs = atof(optarg);
            break;
        case 'f':
            g.off = atof(optarg);
            break;
        case 's':
            g.snr_db = atof(optarg);
            break;
        default:
            usage(argv[0]);
        }
    }
    if (argc - optind != 1)
        usage(argv[0]);
    g.f = fopen(argv[optind], "wb");
    if (!g.f) {
        perror(argv[optind]);
        return 1;
    }

    scn_init(&s, on_slot, &g, 0x1234567);
    ntraffic = scn_run_reference(&s);
    if (g.fmt == OUT_CF32 || g.fmt == OUT_CS16)
        render(&g, 1);
    fclose(g.f);
    free(g.sym);
    free(g.amp);
    fprintf(stderr, "reference scenario: %u traffic slots\n", ntraffic);
    return 0;
}
