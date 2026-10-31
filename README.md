# libtetra

TETRA library focused on 32-bit MCUs.

## Features

- **DMO** (EN 300 396): synchronisation, DMAC, call control, channel
  state, traffic, TPNI, short data including fragmented messages.
- **TMO downlink** (EN 300 392-2, pi/4-DQPSK): synchronisation, SYSINFO,
  AACH, MAC (association, fragmentation), LLC, MLE (neighbour cells,
  network time), MM (registration, group attachment), CMCE calls, traffic,
  SDS and status. Calls on other carriers can be followed through a
  retune callback.
- **TMO uplink**: control and normal uplink bursts (SCH/HU, SCH/F, STCH),
  MAC (random access, fragmentation), U-SETUP, U-TX DEMAND and other call
  control, SDS and status, traffic. Uplink encryption is reported, not
  decrypted.
- SDS-TL / simple text decoding to UTF-8 (GSM 7-bit, ISO 8859-1, UCS-2),
  LIP location reports (TS 100 392-18-1) and location system messages
  (NMEA).
- Speech channel decoding (EN 300 395-2): traffic slots to 137-bit
  speech frames with bad frame indicator, ready for OpenACELP.
- TX building blocks (channel encoding, burst assembly) for later use.
- TEA1/TEA2/TEA3 byte-oriented keystream generation and TMO IV construction.

The TEA API exposes keystream generation, TB5 derivation and a TMO receiver
key-provider interface. TMO signalling and speech can be decrypted when the
application provides the correct provisioned key. Also not supported:
D8PSK/QAM, DM-GATE operation, transmitting through a DM-REP.
Circuit mode data is delivered undecoded.

## TEA keystream generation

The `tetra_crypto.h` API generates TEA1, TEA2, or TEA3 keystream bytes from
an 80-bit ECK and a 29-bit IV. `tetra_tea_build_iv()` assembles the TMO IV
from the slot, frame, multiframe, hyperframe extension, and link direction.
The caller remains responsible for key management when using the primitive
API directly. Consume generated bits most-significant-bit first. For receiver
integration, see the TMO air-interface decryption section below.

```c
#include <tetra/tetra.h>

struct tetra_time time = { .tn = 1, .fn = 1, .mn = 1 };
uint8_t eck[TETRA_TEA_KEY_BYTES] = { 0 }; /* replace with the active ECK */
uint8_t keystream[54];
uint32_t iv;

if (tetra_tea_build_iv(&time, hyperframe, TETRA_TEA_DOWNLINK, &iv) &&
    tetra_tea_generate(TETRA_TEA1, iv, eck, sizeof(keystream), keystream)) {
    /* Use keystream bytes, MSB first, to encrypt/decrypt the MAC fields. */
}
```

The TEA1/2/3 source files and the TB5 implementation are derived from
Osmocom `osmo-tetra` and retain their AGPL-3.0-or-later licensing and relevant
copyright attribution. The AGPL text is included as `LICENSE-AGPL-3.0`, other
libtetra files retain their GPL-3.0-or-later licensing. ETSI EN 300 392-7 states
that the TETRA standard algorithms are available on a restricted basis. Users
must comply with the
applicable ETSI TS 101 053 terms and undertakings.

## TMO air-interface decryption

TMO TEA1/TEA2/TEA3 decryption is enabled by supplying a base-key provider.
The callback selects the applicable provisioned 80-bit CK for the current
cell (normally the CCK or SCK) and returns the network key-stream generator.
libtetra applies TB5 using the SYSINFO main carrier, location area and colour
code, then uses the cell hyperframe and current TDMA position to construct the
cipher IV. The application remains responsible for securely storing keys and
selecting the correct key for the cell and traffic context.

```c
static bool get_tmo_key(void *user,
                        const struct tetra_tmo_key_request *request,
                        enum tetra_tea_algorithm *algorithm,
                        uint8_t ck[TETRA_TEA_KEY_BYTES])
{
    struct my_keystore *store = user;

    /* Select by cell/network metadata. request->address may be ciphered. */
    return my_keystore_lookup(store, request, algorithm, ck);
}

/* After tetra_tmo_init(): */
tetra_tmo_set_key_provider(&rx, get_tmo_key, &keystore);
```

The callback must return CK, not an already TB5-derived ECK. The MAC address
in the request is the on-air value and may itself be encrypted, so a provider
must not assume it is a plaintext SSI/GSSI. CCK ID is exposed when present,
but is not required for SCK-based selection. The raw two-bit MAC
encryption-mode field is also passed to the provider: `2` indicates an even
key identifier, `3` an odd key identifier. The field is
marked invalid for traffic when no MAC header value was observed. The traffic
speech decoder consumes the keystream supplied with `TETRA_EV_TRAFFIC` and
removes encryption before validating the speech-frame parity. MAC signalling
is decrypted before LLC/L3 parsing. If the provider has no key, the receiver
continues reporting the encrypted PDU/traffic state without attempting to
parse ciphertext. The provider is responsible for the relevant CCK/SCK key
selection. Individual DCK/GCK derivation and key-file parsing are not
implemented by this interface.

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

To follow calls allocated to other carriers of the cell, give a retune
callback. It is asked to tune to the call's carrier and back to the main
carrier when the call ends; return false to stay where you are:

```c
static bool on_retune(void *user, const struct tetra_retune *r)
{
    if (r->reason == TETRA_RETUNE_CALL && !r->addressed)
        return false;               /* only follow our own calls */
    return radio_set_rx_hz(r->freq_hz) == 0;
}

cfg.retune = on_retune;
```

To listen to the uplink of a cell, synchronise to its downlink first (or
call `tetra_tmo_set_uplink_cell()`), then tune to `cell->ul_freq_hz` and
call `tetra_tmo_resync()`. Uplink events have `ev->uplink` set; the sending
MS is in `src_ssi`.

## Footprint

About 24 KB of flash. RAM is the context: 5.4 KB (DMO) or 6.7 KB (TMO),
plus 1.6 KB for the speech decoder workspace. Receiver sizing is configured
with the CMake cache options `TETRA_TMO_MAX_SDU_BITS`,
`TETRA_DMO_MAX_SDU_BITS`, and `TETRA_TMO_MAX_CALLS`. Stack use stays below
1 KB plus the callback. `-DTETRA_UPLINK_DECODE=OFF` leaves out the uplink
decoder.

## Build and test

```bash
cmake -B build
cmake --build build
ctest --test-dir build

# Crypto is disabled by default. Enable TEA1/2/3, TB5 and TMO decryption:
cmake -B build-crypto -DTETRA_ENABLE_CRYPTO=ON
cmake --build build-crypto

# Explicitly disable crypto in an existing build directory:
cmake -B build-no-crypto -DTETRA_ENABLE_CRYPTO=OFF
cmake --build build-no-crypto

cmake -B build_m4 -DCMAKE_TOOLCHAIN_FILE=cmake/arm-cortex-m4.cmake  # or m33
cmake --build build_m4
```

Tests need Catch2 v3. Receiver sizing and feature settings that affect the
public context layout are configured through CMake, for example
`-DTETRA_TMO_MAX_CALLS=8 -DTETRA_UPLINK_DETECT=OFF`. The generated public
`tetra_config.h` fixes these values for both the library and its consumers,
conflicting compiler `-D` overrides are rejected. Inside another CMake
project, use `add_subdirectory(libtetra)` and link the `tetra` target. Installed
builds export `tetra::tetra` via `find_package(libtetra CONFIG)`.

## Tools

- `tetra_demod`: I/Q recording to bits (`-f` carrier offset in Hz).
- `tetra_rx`: prints decoded events, optionally writes speech frames.
- `tetra_gen`: synthetic DMO signal (bits or I/Q).

```bash
build/tetra_demod -f -20000 -o soft capture.wav capture.soft
build/tetra_rx -m tmo -t soft -q -s speech.ser capture.soft
build/tetra_rx -m tmo -t soft -u 235:78:1 uplink.soft   # MCC:MNC:CC
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
