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

**Hardware limitation:** the track/PLAY buttons share one resistor ladder, as
do the volume/rocker buttons. Only one button *per ladder* is reportable at a
time (the 1+4 band is reserved for the DFU failsafe). Cross-ladder chords —
e.g. PLAY + Volume +, FUNCTION + anything — are fully reportable because
FUNCTION is a dedicated GPIO and the two ladders are independent.

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
