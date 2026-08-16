# Stem Tape firmware — Milestone M0

Raw USB-MIDI control surface for the Teenage Engineering × Kanye West SP-1.

The device enumerates as a **class-compliant USB MIDI device** (USB MIDI 2.0
class, MIDI 1.0 channel-voice messages) plus the usual CDC ACM console. It
transmits **physical state only**. There is no looper, no chord detection, no
tap/hold discrimination and no musical LED feedback on the device: the host
(the Stem Tape web app) owns all interpretation.

## Reused unchanged from the SP-1 Tape Looper firmware (`firmware/`)

- board definition `boards/teenageengineering/stem_player`
- LED pin map + always-dim soft-PWM renderer (zero-latency TIMER3 ISR)
- BTN_COM ladder rail, 2× oversampled ADC read, verified voltage bands
- fader ADC channels and ±8-count deadband
- power button P0.27, 2.5 s hold-to-power-off with LED countdown, SYSTEM_OFF
  wake arming, Track1+Track4 ≈1.2 s DFU failsafe
- 4 s watchdog, fatal-fault → clean reboot
- device_next USB stack + the shared `sample_usbd` bring-up helper

## MIDI map (channel 1, UMP group 0)

| Control | Message |
| --- | --- |
| Track 1–4 | Note On/Off 36, 37, 38, 39 |
| PLAY | Note On/Off 40 |
| FUNCTION (power button) | Note On/Off 41 |
| Volume + / − | Note On/Off 42 / 43 |
| Rocker FWD / RWD | Note On/Off 44 / 45 |
| Faders 1–4 | CC 20, 21, 22, 23 (0–127) |
| Battery level | CC 24 (≤ 1 Hz, on change) |
| Host (re)connect | CC 123 All Notes Off, then current state resent |

Button down = velocity 127, button up = velocity 0.

## Pinned baseline

Every copied power, DFU, ADC-band, LED and GPIO constant comes from
`firmware/src/main.c` at commit **a8dd127ba1d595e54f92503a0bd75eabca86334d**
(2026-08-15 08:21:32 +0000). Each constant in `src/main.c` carries an inline
`[looper a8dd127:<line>]` citation. The Tape Looper target is unchanged.

## Shared-ladder decoding (bitmask, measured bands only)

The SP-1 has no independent digital input per button: PLAY + Track 1–4 share
SAADC AIN0, and Vol−/Vol+/rocker share AIN1. A chord is therefore a *single
new voltage*, not two readings.

The pinned looper revision contains exactly one measured chord band —
Track 1 + Track 4, raw 1280–1390, the DFU failsafe. All other chord bands are
**UNMEASURED**. M0 decodes into a bitmask against a measured-band table; an
unmeasured reading emits **no MIDI**, holds that ladder's previous stable
bits, and is surfaced on the CDC diagnostic stream for capture. Note On/Off
transitions are derived by XOR-diffing the previous and current *stable*
masks. FUNCTION (P0.27) is a real GPIO and stays independently detectable.

Deliberate difference from the looper: `decode_tracks()` maps 950–1499 to
Track 4 and everything ≥1500 to PLAY. M0 narrows the single-button bands to
their measured neighbourhoods (T4 950–1279, PLAY 1600–2047; VOL+ 1600–2047)
so that a chord voltage is never reported as an unrelated single button.

### ADC bands still requiring physical measurement

1. PLAY + each Track button (AIN0).
2. Volume − + Volume + (AIN1).
3. Rocker direction + each Volume button (AIN1).
4. FUNCTION with every other control (verifies the GPIO stays independent).
5. Press/release order in both directions for each chord.

## CDC ACM diagnostics

The composite device is **MIDI2 + CDC ACM, no UAC2**. When the serial port is
opened with DTR asserted, M0 prints a banner and then one line **only when a
ladder value changes** (±3-count hysteresis, ≥40 ms apart):

```
AIN0= 213 AIN1=   0 dec=T1           stable=T1           unmeas=0
```

fields: raw AIN0, raw AIN1, decoded mask (or `UNMEASURED`), debounced stable
mask, cumulative unmeasured-reading count. No continuous flooding.

## MIDI compatibility status

Zephyr 4.3.1 implements only the USB MIDI 2.0 class (`CONFIG_USBD_MIDI2_CLASS`);
**the USB-MIDI 1.0 alternate mode is not implemented in that revision.** M0
sends MIDI 1.0 Channel Voice messages inside UMP message type 2, which hosts
translate back to plain MIDI 1.0 events. CC123 + the full-state resend fire
only from `ready_cb`, i.e. after the host has actually selected the
operational alternate setting.

Compatibility with the Stem Tape bridge is **unverified** until the artifact
is flashed and checked on: macOS Audio MIDI Setup / MIDI Monitor, Chrome Web
MIDI, and the native CoreMIDI wrapper on a physical iPhone or iPad.

## Safety

UAC2 is disabled in this diagnostic target only. The eMMC driver is not
compiled in: no mount, no format, no write — stored Tape Looper audio is
untouched. The Track 1 + Track 4 → UF2 bootloader recovery from the pinned
revision is preserved and checked on the raw ADC code before decoding. No
automatic flashing.

## Boot signature

Two quick flashes of all four track LEDs (90 ms on / 110 ms off) distinguish
Stem Tape M0 from the stock looper at power-on.

## Building

No Zephyr toolchain is available in the Lovable sandbox (`west update` cannot
run there), so builds happen in CI:

```
west build -p always -b stem_player firmware/stemtape -d build-stemtape \
  -- -DBOARD_ROOT="$PWD/firmware"
```

Zephyr v4.3.1, Zephyr SDK 0.17.4, `arm-zephyr-eabi`. The GitHub Actions job
`stemtape-m0` in `.github/workflows/firmware.yml` runs exactly this and
publishes `stemtape_m0.bin` (path `build-stemtape/zephyr/stemtape_m0.bin`)
with its SHA-256 in the job summary.

## Flashing

Hold Track 1 + Track 4 while plugging in (or ≈1.2 s while running) to enter
the UF2 bootloader, then copy the built image as usual for the SP-1.

## Firmware-safety corrections (this revision)

* **Early T1+T4 bootloader escape** — `early_dfu_escape()` runs immediately
  after `controls_init()` and **before** `boot_signature()`,
  `sample_usbd_init_device()`, `usbd_enable()` and every other USB call. It
  reads raw AIN0; outside 1280..1390 it returns instantly (no added boot
  delay); inside, the reading must stay in band continuously for 1200 ms
  (re-sampled every 10 ms, all enabled watchdog reload channels fed) before
  the existing `enter_dfu()` is called. Release or a failed ADC read returns
  to normal startup. Recovery therefore works even if USB init would later
  fail. The in-loop combo check is unchanged.
* **Watchdog** — `wdt_init()` first reads `NRF_WDT->RUNSTATUS`. If the
  bootloader already started the WDT the configuration is locked and only
  feeding happens. Otherwise `wdt_install_timeout()` and `wdt_setup()` return
  values are both captured (`g_wdt_install_rc`, `g_wdt_setup_rc`) and never
  assumed to have succeeded. `feed_wdt()` reloads exactly the channels
  enabled in `RREN` (all channels when none is enabled yet). The CDC
  diagnostic banner prints `pre_running`, `ours`, both return codes, `RREN`
  and `RUNSTATUS`.

Physical approval still requires a debugger-connected SP-1 test proving the
early T1+T4 recovery path before any stock device is used.
