/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * Host-side receiver: reads a demodulated bit stream and prints the events
 * produced by libtetra. Optionally writes traffic frames in the 690-word
 * format of the ETSI reference channel decoder.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tetra/tetra.h>

enum in_fmt { IN_DIBIT, IN_BIT, IN_SOFT };

static FILE *tch_out;
static FILE *speech_out;
static int quiet_traffic;
static unsigned n_traffic;
static unsigned n_frames, n_frames_ok;
static struct tetra_speech_ws speech_ws;

/* ETSI serial format: per 30 ms frame a BFI word and 137 bit words. */
static void write_speech(const struct tetra_traffic *t)
{
    struct tetra_speech sp;

    n_frames_ok += (unsigned)tetra_speech_decode(&speech_ws, t, &sp);
    for (unsigned f = 0; f < 2; f++) {
        int16_t w[1 + TETRA_SPEECH_BITS];

        w[0] = sp.bfi[f];
        for (unsigned i = 0; i < TETRA_SPEECH_BITS; i++)
            w[1 + i] = sp.bits[f][i];
        fwrite(w, sizeof(w[0]), 1 + TETRA_SPEECH_BITS, speech_out);
        n_frames++;
    }
}

static const char *call_what[] = {
    "SETUP",      "CONNECT", "TX-GRANTED", "TX-CEASED",
    "LATE-ENTRY", "INFO",    "RELEASE",
};

static const char *chan_state[] = { "FREE", "OCCUPIED", "RESERVED",
                                    "SDS-OCCUPIED" };

static void print_time(const struct tetra_event *ev)
{
    if (ev->mode == TETRA_MODE_TMO)
        printf("[%02u/%02u/%u] ", ev->time.mn, ev->time.fn, ev->time.tn);
    else
        printf("[%02u/%u] ", ev->time.fn, ev->time.tn);
}

static void print_sds(const struct tetra_sds *s)
{
    struct tetra_sds_text t;
    char txt[512];

    printf("SDS from %u to %u%s: ", s->src_ssi, s->dst_ssi,
           s->complete ? "" : " (incomplete)");
    if (s->type == TETRA_SDS_STATUS) {
        printf("status %u\n", s->status);
        return;
    }
    if (s->type != TETRA_SDS_UDD4) {
        printf("UDD%d %u bits\n", s->type + 1, s->nbits);
        return;
    }
    if (tetra_sds_parse_text(s, &t) == 0 &&
        tetra_sds_text_utf8(&t, txt, sizeof(txt)) >= 0) {
        printf("text (pid 0x%02x, tcs %u): \"%s\"\n", t.protocol_id, t.coding,
               txt);
        return;
    }
    printf("UDD4 pid 0x%02x, %u bits\n", s->nbits >= 8 ? s->data[0] : 0,
           s->nbits);
}

static void on_event(void *user, const struct tetra_event *ev)
{
    (void)user;

    if (ev->type == TETRA_EV_TRAFFIC) {
        n_traffic++;
        if (tch_out) {
            int16_t frame[690];

            tetra_traffic_to_etsi(&ev->u.traffic, frame);
            fwrite(frame, sizeof(frame[0]), 690, tch_out);
        }
        if (speech_out)
            write_speech(&ev->u.traffic);
        if (quiet_traffic)
            return;
    }

    print_time(ev);
    switch (ev->type) {
    case TETRA_EV_SYNC:
        printf("SYNC %s\n", ev->u.sync.locked ? "acquired" : "lost");
        break;
    case TETRA_EV_CELL: {
        const struct tetra_cell *c = ev->u.cell;

        printf("CELL MCC %u MNC %u CC %u", c->mcc, c->mnc, c->colour_code);
        if (c->have_sysinfo)
            printf(" LA %u DL %u Hz carrier %u HF %u services 0x%03x%s",
                   c->location_area, c->dl_freq_hz, c->main_carrier,
                   c->hyperframe, c->service_details,
                   c->air_encryption ? " (air enc)" : "");
        printf("\n");
        break;
    }
    case TETRA_EV_CHANNEL:
        printf("CHANNEL %s master %u prio %u\n",
               chan_state[ev->u.channel.state], ev->u.channel.master_ssi,
               ev->u.channel.priority);
        break;
    case TETRA_EV_CALL: {
        const struct tetra_call *k = &ev->u.call;

        printf("CALL %s id %u um %u ts 0x%x dst %u%s src %u%s cause %u\n",
               call_what[k->what], k->call_id, k->usage_marker, k->timeslots,
               k->dst_ssi, k->group ? " (group)" : "", k->src_ssi,
               k->encrypted ? " ENCRYPTED" : "", k->cause);
        break;
    }
    case TETRA_EV_SDS:
        print_sds(&ev->u.sds);
        break;
    case TETRA_EV_TRAFFIC: {
        const struct tetra_traffic *t = &ev->u.traffic;

        printf("TRAFFIC um %u call %u talker %u halves %d/%d\n",
               t->usage_marker, t->call_id, t->talker_ssi, t->half[0],
               t->half[1]);
        break;
    }
    case TETRA_EV_PDU:
        printf("PDU proto %d type %u addr %u %u bits%s\n", ev->u.pdu.proto,
               ev->u.pdu.type, ev->u.pdu.addr, ev->u.pdu.nbits,
               ev->u.pdu.encrypted ? " encrypted" : "");
        break;
    }
}

static void usage(const char *p)
{
    fprintf(stderr,
            "usage: %s [-m tmo|dmo] [-t dibit|bit|soft] [-w tch.out]\n"
            "          [-s speech.ser] [-q] input\n"
            "  -w  write traffic frames in ETSI 690-word format\n"
            "  -s  write decoded speech frames in ETSI serial format\n"
            "      (BFI + 137 bits per frame, 16-bit words)\n"
            "  -q  do not print traffic events\n",
            p);
    exit(2);
}

static struct tetra_tmo_rx tmo;
static struct tetra_dmo_rx dmo;

int main(int argc, char **argv)
{
    struct tetra_rx_config cfg;
    enum in_fmt fmt = IN_DIBIT;
    int is_dmo = 0, opt;
    uint8_t buf[4096];
    size_t n;
    FILE *f;

    while ((opt = getopt(argc, argv, "m:t:w:s:q")) != -1) {
        switch (opt) {
        case 'm':
            is_dmo = !strcmp(optarg, "dmo");
            break;
        case 't':
            if (!strcmp(optarg, "dibit"))
                fmt = IN_DIBIT;
            else if (!strcmp(optarg, "bit"))
                fmt = IN_BIT;
            else if (!strcmp(optarg, "soft"))
                fmt = IN_SOFT;
            else
                usage(argv[0]);
            break;
        case 'w':
            tch_out = fopen(optarg, "wb");
            if (!tch_out) {
                perror(optarg);
                return 1;
            }
            break;
        case 's':
            speech_out = fopen(optarg, "wb");
            if (!speech_out) {
                perror(optarg);
                return 1;
            }
            break;
        case 'q':
            quiet_traffic = 1;
            break;
        default:
            usage(argv[0]);
        }
    }
    if (argc - optind != 1)
        usage(argv[0]);

    f = fopen(argv[optind], "rb");
    if (!f) {
        perror(argv[optind]);
        return 1;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.cb = on_event;
    cfg.promiscuous = true;
    if (is_dmo)
        tetra_dmo_init(&dmo, &cfg);
    else
        tetra_tmo_init(&tmo, &cfg);

    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (is_dmo) {
            if (fmt == IN_DIBIT)
                tetra_dmo_push_dibits(&dmo, buf, n);
            else if (fmt == IN_BIT)
                tetra_dmo_push_bits(&dmo, buf, n);
            else
                tetra_dmo_push_soft(&dmo, (const int8_t *)buf, n);
        } else {
            if (fmt == IN_DIBIT)
                tetra_tmo_push_dibits(&tmo, buf, n);
            else if (fmt == IN_BIT)
                tetra_tmo_push_bits(&tmo, buf, n);
            else
                tetra_tmo_push_soft(&tmo, (const int8_t *)buf, n);
        }
    }
    fclose(f);
    if (tch_out)
        fclose(tch_out);
    fprintf(stderr, "%u traffic slots\n", n_traffic);
    if (speech_out) {
        fclose(speech_out);
        fprintf(stderr, "%u speech frames, %u passed the CRC\n", n_frames,
                n_frames_ok);
    }
    return 0;
}
