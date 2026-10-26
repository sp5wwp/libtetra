# libtetra

TETRA library focused on 32-bit MCUs.

## Features

- **DMO** (EN 300 396): synchronisation, DMAC, call control, channel
  state, traffic, TPNI, short data including fragmented messages.
- **TMO downlink** (EN 300 392-2, pi/4-DQPSK): synchronisation, SYSINFO,
  AACH, MAC (association, fragmentation), LLC, MLE, CMCE calls, traffic,
  SDS and status. TMO uplink signals are recognised (not decoded).
- SDS-TL / simple text decoding to UTF-8 (GSM 7-bit, ISO 8859-1, UCS-2).
- Speech channel decoding (EN 300 395-2): traffic slots to 137-bit
  speech frames with bad frame indicator, ready for OpenACELP.
- TX building blocks (channel encoding, burst assembly) for later use.

Not supported: air interface encryption, TMO uplink, D8PSK/QAM,
DM-REP/DM-GATE operation. Circuit mode data is delivered undecoded.

## Usage

```c
#include <tetra/tetra.h>

static struct tetra_dmo_rx rx;   /* or struct tetra_tmo_rx */

static void on_event(void *user, const struct tetra_event *ev)
{
    (void)user;

    switch (ev->type) {
    case TETRA_EV_CALL:    /* ev->u.call */
        break;
    case TETRA_EV_SDS:     /* ev->u.sds, see tetra_sds.h for text */
        break;
    case TETRA_EV_TRAFFIC: /* ev->u.traffic.bits: 432 soft bits */
        break;
    default:
        break;
    }
}

void radio_init(void)
{
    struct tetra_rx_config cfg = {
        .cb = on_event,
        .issi = 1234,
        .promiscuous = true,
    };

    tetra_dmo_init(&rx, &cfg);
}

/* Demodulated symbols, one dibit per byte: B(2k-1) << 1 | B(2k). */
void radio_symbols(const uint8_t *dibits, size_t n)
{
    tetra_dmo_push_dibits(&rx, dibits, n);
}
```

Soft bits (`tetra_*_push_soft`) improve decoding when the demodulator can
provide them.

Speech frames for an ACELP decoder such as [OpenACELP](https://github.com/sp5wwp/OpenACELP):

```c
static struct tetra_speech_ws ws;
struct tetra_speech sp;

tetra_speech_decode(&ws, &ev->u.traffic, &sp);
for (int f = 0; f < 2; f++)
    oa_dec_frame(&dec, sp.bits[f], sp.bfi[f], pcm); /* 240 samples each */
```

To save CPU time while following a TMO call, decode only its timeslots:

```c
case TETRA_EV_CALL:
    if (ev->u.call.what == TETRA_CALL_CONNECT)
        tetra_tmo_set_slots(&rx, ev->u.call.timeslots);
    else if (ev->u.call.what == TETRA_CALL_RELEASE)
        tetra_tmo_set_slots(&rx, TETRA_TS_ALL);
    break;
```

## Footprint

About 19 KB of flash. RAM is the context: 5.4 KB (DMO) or 6.5 KB (TMO),
sizes adjustable in `tetra_config.h`, plus 1.6 KB for the speech decoder
workspace. Stack use stays below 1 KB plus the callback.

## Build and test

```bash
cmake -B build
cmake --build build
ctest --test-dir build

cmake -B build_m4 -DCMAKE_TOOLCHAIN_FILE=cmake/arm-cortex-m4.cmake  # or m33
cmake --build build_m4
```

Tests need Catch2 v3. Inside another CMake project, use
`add_subdirectory(libtetra)` and link the `tetra` target.

## Tools

- `tetra_demod`: I/Q recording to bits (`-f` carrier offset in Hz).
- `tetra_rx`: prints decoded events, optionally writes speech frames.
- `tetra_gen`: synthetic DMO signal (bits or I/Q).

```bash
build/tetra_demod -f -20000 -o soft capture.wav capture.soft
build/tetra_rx -m tmo -t soft -q -s speech.ser capture.soft
oa_decode speech.ser speech.raw    # OpenACELP: 8 kHz 16-bit PCM
```

## Layout

```
include/tetra/   public API
src/l1/          bursts, training sequences, bit history
src/l2/          channel coding, lower MAC, TMO and DMO upper MAC
src/l3/          DMO call control, TMO LLC/MLE/CMCE, SDS
tests/           Catch2 unit tests and test vectors
tools/           host tools
```

## License

GPL-3.0-or-later.
