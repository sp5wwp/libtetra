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
- TA61 identity decryption: recovers the cleartext SSI from the encrypted
  short identity (ESI) carried in TMO MAC addresses, via a CCK provider.

The TEA API exposes keystream generation, TB5 derivation, TA61 identity
decryption and a TMO receiver key-provider interface. TMO signalling and
speech can be decrypted when the application provides the correct provisioned
key, and encrypted MAC identities resolved when it provides the cell CCK.
Also not supported: D8PSK/QAM, DM-GATE operation, transmitting through a
DM-REP. Circuit mode data is delivered undecoded.

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

The TEA1/2/3 source files, the TB5 implementation and the TA61/Hurdle
primitive (`src/crypto/taa1.c`, `src/crypto/hurdle.c`, vendored unchanged)
are derived from Osmocom `osmo-tetra` and retain their AGPL-3.0-or-later
licensing and relevant copyright attribution. The AGPL text is included as
`LICENSE-AGPL-3.0`, other libtetra files retain their GPL-3.0-or-later
licensing. ETSI EN 300 392-7 states
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

### Identity decryption (TA61)

On networks that encrypt identities, the SSI in a TMO MAC address is the
encrypted short identity (ESI). Supplying a CCK provider lets the receiver
recover the cleartext SSI with TA61, so call tracking, SDS, the addressed
test and carrier following see the real identity:

```c
static bool get_cck(void *user, const struct tetra_cell *cell,
                    uint8_t cck[TETRA_TEA_KEY_BYTES])
{
    return my_keystore_cck(user, cell->mcc, cell->mnc, cell->cck_id, cck);
}

tetra_tmo_set_cck_provider(&rx, get_cck, &keystore);
```

The CCK is used directly (not through TB5) and is independent of the traffic
key provider. `tetra_ta61_decrypt_ssi()` exposes the primitive on its own.
Decryption is applied to SSI-bearing MAC address types. USSI, which is the
explicitly unencrypted form, is left as received.

TA61 builds on the Hurdle block cipher. These two files are **not bundled**:
copy them unchanged from an `osmo-tetra` checkout into `src/crypto/` before
building with crypto enabled:

```
cp osmo-tetra/src/crypto/{taa1.c,taa1.h,hurdle.c,hurdle.h} src/crypto/
```

`hurdle.c` reads and writes its 8-byte cipher state with word-sized casts
over `uint8_t` buffers. This needs care on ARMv7-M / ARMv8-M (Cortex-M3/M4/
M33):

- **Strict aliasing.** The same buffers are accessed as both `uint8_t` and
  `uint32_t`, which is undefined behaviour and can be miscompiled at `-O2`.
  The CMake build compiles the vendored files with `-fno-strict-aliasing`.
  Reproduce that flag in any other build system.
- **Alignment.** At `-O2` the two adjacent word casts in `HURDLE_encrypt`
  compile to `LDRD`/`STRD`, which fault on an unaligned address on Cortex-M
  regardless of `CCR.UNALIGN_TRP` (unlike a single `LDR`). The state buffers
  are `uint8_t[]` locals, so a HardFault depends on stack placement and can
  appear with a compiler change. For a dependable MCU build, replace the
  four `*(uint32_t *)` accesses of `abInput`/`abOutput` in `HURDLE_encrypt`
  with `memcpy` (the compiler lowers it to safe byte accesses), or make
  those buffers word-aligned. Cortex-A (e.g. A55) and other AArch64/x86
  targets tolerate the unaligned access and are unaffected.

The compile-time `__BYTE_ORDER`/`__LITTLE_ENDIAN` switch in these files also
resolves to the little-endian path only by accident when those macros are
undefined (as on bare-metal toolchains): both sides evaluate to 0. That is
correct for little-endian Cortex targets but is worth pinning explicitly if
you build for anything else.

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

A TMO call can be moved to another carrier of the cell. The retune callback
lets the receiver follow it, see Following calls across carriers below.

To listen to the uplink of a cell, synchronise to its downlink first, or
call `tetra_tmo_set_uplink_cell()`, then tune to `cell->ul_freq_hz` and call
`tetra_tmo_resync()`. Uplink events have `ev->uplink` set and the sending MS
is in `src_ssi`.

## Following calls across carriers

A TMO cell can move a call to one of its other carriers. Set `retune` in
`tetra_rx_config` and the receiver asks the application to tune there, and
back again when the call is over. It never touches the radio itself. DMO has
no carrier allocation and never calls it.

```c
static bool on_retune(void *user, const struct tetra_retune *r)
{
    if (r->reason == TETRA_RETUNE_CALL && !r->addressed)
        return false;
    return radio_set_rx_hz(r->freq_hz) == 0;
}
```

The callback is asked for one of two things, given in `r->reason`.

- `TETRA_RETUNE_CALL` comes when a channel allocation for a known call
  points to a carrier other than the one being received. `call_id`,
  `dst_ssi`, `timeslots` and `addressed` describe the call. Uplink only
  allocations are ignored.
- `TETRA_RETUNE_MAIN` asks to go back to the main carrier of the cell. It
  comes when the followed call is released, when it has been silent for
  about a minute, or when lock is lost while away from the main carrier. It
  is not sent if the radio is already on the main carrier.

Every request carries the downlink frequency `freq_hz`, the matching uplink
frequency `ul_freq_hz` (0 if the duplex spacing is unknown) and the carrier
number.

Return true only once the radio receives `freq_hz`, and keep pushing
samples. The carriers of a cell share the TDMA timing, so the receiver stays
locked across the switch. Return false to stay where you are. Allocations
are repeated on air, so a declined call may be offered again.

Some details worth knowing.

- Only calls for your own identities are offered unless `promiscuous` is
  set. Use `addressed` to filter in promiscuous mode.
- One call is followed at a time. While following it, allocations for other
  calls are not offered.
- The frequency comes from the cell SYSINFO, so no request is made before
  the first SYSINFO unless the allocation carries its own band information.
- The callback runs in the context that pushes samples, like the event
  callback. Keep it short. It may call `tetra_tmo_set_slots()` with
  `r->timeslots` to decode only the slots of the followed call.
- If you change frequency on your own, call `tetra_tmo_resync()`. The
  receiver then re-acquires where it is and does not ask to go back.

## Events

Everything the receiver decodes arrives through the one callback in
`tetra_rx_config`. Each `struct tetra_event` carries a `type`, the `mode`
(TMO or DMO), the TDMA `time`, and an `uplink` flag. The payload is in the
`u` union and is valid only during the callback, so copy anything you keep.

- `TETRA_EV_SYNC`, burst lock gained or lost, in `u.sync.locked`.
- `TETRA_EV_CELL`, TMO cell information after SYNC or SYSINFO, in `u.cell`.
- `TETRA_EV_NETWORK`, the TMO network broadcast with neighbour cells and
  network time, in `u.network`.
- `TETRA_EV_CALL`, call control from set-up through release, in `u.call`.
- `TETRA_EV_SDS`, a short data or status message, in `u.sds`.
- `TETRA_EV_TRAFFIC`, one traffic slot of soft bits for the codec, in
  `u.traffic`.
- `TETRA_EV_MM`, TMO mobility management such as registration and group
  attachment, in `u.mm`.
- `TETRA_EV_CHANNEL`, a DMO channel state change, in `u.channel`.
- `TETRA_EV_PDU`, any other layer 2 or 3 PDU for monitoring, in `u.pdu`.
- `TETRA_EV_SIGNAL`, a recognised signal that cannot be decoded, such as a
  TMO uplink burst seen while not synchronised.

Set `promiscuous` in the config to see every call. Leave it off and set
`issi` and the `groups` array to hear only about your own identities. Call,
SDS and traffic events then carry an `addressed` flag.

## Short data, text and location

An SDS event hands you the raw short data in `u.sds`. For the common
payloads `tetra_sds.h` turns the bits into something usable. Each parser
returns 0 when the message is of that kind and fills a struct that points
back into the event data.

- `tetra_sds_parse_text` then `tetra_sds_text_utf8` give a NUL terminated
  UTF-8 string. GSM 7-bit, ISO 8859-1 and UCS-2 convert exactly.
- `tetra_sds_parse_location` then `tetra_sds_location_ascii` give NMEA
  location sentences as ASCII text.
- `tetra_sds_parse_lip` decodes a Location Information Protocol report into
  `struct tetra_lip`, with latitude and longitude in units of 1e-7 degrees.

```c
struct tetra_sds_text t;
char text[160];
if (tetra_sds_parse_text(&ev->u.sds, &t) == 0)
    tetra_sds_text_utf8(&t, text, sizeof(text));
```

## Cell and network information

After the first SYNC and SYSINFO the current cell is in `u.cell` and from
`tetra_tmo_cell()`. It holds the mobile country and network codes, colour
code, location area, main carrier, and the downlink and uplink frequencies
in Hz as `dl_freq_hz` and `ul_freq_hz`. `TETRA_EV_NETWORK` and
`tetra_tmo_network()` add neighbour cells, each with its own frequency, and
the network time. `tetra_carrier_hz()` and `tetra_uplink_hz()` compute a
carrier frequency from the band, carrier number and duplex fields when you
need one outside an event.

## Footprint

About 24 KB of flash for the core receiver, more with crypto or the uplink
decoder enabled. RAM is the receiver context, measured for Cortex-M at
5.4 KB for DMO, 6.8 KB for TMO, or 7.3 KB for TMO with crypto, plus 1.6 KB
for the speech decoder workspace. Receiver sizing is configured
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

- `tetra_demod`: I/Q recording to bits (`-f` carrier offset in Hz). It
  re-acquires at each burst, so TMO uplink and DMO recordings work too.
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
