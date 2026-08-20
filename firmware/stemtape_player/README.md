# Stem Tape Player — standalone firmware (release skeleton)

The beginning of the standalone, four-stem Stem Tape player: a dedicated
Zephyr app target (separate from `firmware/stemtape`, the M0 MIDI control-
surface diagnostic, and `firmware/`, the SP-1 Tape Looper) that boots the
board on its own, with USB entirely optional.

## What this pass actually delivers (real, built, tested)

- **Watchdog and power button hold-to-off** — reused verbatim from the
  pinned Tape Looper (`firmware/src/main.c`, commit `a8dd127`) and the M0
  target, checked before any other subsystem so recovery works even if
  something later hangs. The Tape Looper's in-firmware Track1+Track4 DFU
  combo is **removed** (product ruling): Track 1 and Track 4 are ordinary
  performance controls on this instrument, and a gesture that reset the
  device out of the running firmware mid-song was never compatible with
  playing it. Recovery is unaffected — see below.

## Entering the UF2 bootloader

**With the SP-1 off, hold Track 1 + Track 4 and plug in USB. One track light
comes on — that is bootloader mode.** (Owner-confirmed procedure.)

That scan lives in the bootloader image and runs before this firmware is
entered at all, so nothing in this application can affect it. Note the cue is
**one** track LED; the removed in-firmware `enter_dfu()` lit **all four** —
they were always two different code paths, and the surviving one is not ours
to break.

`firmware/README.md`'s rule — "must always offer a path back to the
bootloader … do not remove those" — still holds, satisfied twice over: the
boot-time combo above, and FUNCTION held 2.5 s → `power_off()` →
`SYSTEM_OFF`, which is unchanged.
- **The eight-LED PWM renderer** (`led_duty.c`, `led_render.c`,
  `led_render_policy.c`) — byte-for-byte the same hardware driver already
  proven and CI-built for the M0 target — now driving a NEW **local
  semantic pattern engine** (`src/st_led_pattern.h`/`.c`) instead of M0's
  MIDI-channel-16 protocol. No host connection of any kind is needed to
  animate it.
- **The versioned companion transfer protocol**
  (`src/st_transfer_protocol.h`, `src/st_storage_layout.h`,
  `src/st_transfer.c`) — the full transactional begin/stage/verify/commit/
  abort/delete/init state machine, host-tested against a mocked sector
  backend. See [`docs/stem-tape-transfer-v1.md`](../../docs/stem-tape-transfer-v1.md).
- **The physical gesture grammar** (`src/st_gesture.c`) — idle debounce,
  chord atomicity, fader pickup/crossing, the global loop momentary/latch/
  release grammar, STEM/GLOBAL FX scope open/close/cycle, FX Track
  momentary/latch/unlatch, and the full forward/reverse scrub latch
  grammar (task section 7, items 1-10) — as a PURE, host-tested state
  machine that emits semantic commands. **Not yet wired to real ADC/GPIO
  reads** — see "Deferred" below.
- **The four persistent scrub speeds and the tape-inertia release ramp**
  (`src/st_scrub.c`) — ported from `src/audio/inertia.ts`'s exact
  documented math (`GLOBAL_SCRUB_SPEEDS`, the finite exponential curve,
  the zero-crossing solver), not re-derived.
- **The FX bank/algorithm catalog** (`src/st_fx_catalog.h`) — ported
  verbatim from `src/machine/fx12.ts`'s twelve-algorithm model (Filter/
  Exciter/Dirt-Crusher in TONE, plus MOD/MOTION/SPACE), including the
  exact physical Track-button mapping. The DSP audio processing per
  algorithm is not ported — see "Deferred".

All of the above compiles as pure, portable C and is exercised by
`tests/test_stemtape_player.c` (12,131 checks — see the release report for
the exact count from the CI run) with zero Zephyr/hardware dependency,
following the same host-test discipline established for the M0 target's
`led`/`led_frame`/`led_render_policy` modules.

## Deferred beyond this release

This is a **skeleton that boots and proves its foundations**, not yet a
playable instrument. Explicitly NOT implemented in this pass:

- **The real-time audio engine**: 24-bit stem decode, mixing with
  accumulator headroom/saturation/limiter, per-algorithm FX DSP (the
  Web Audio graphs in `src/audio/fx/banks.ts` are not ported — only the
  selection/routing catalog is), the actual scrub resampler, CS42L42
  headphone and TAS2505 speaker bring-up, and headphone-insertion speaker
  muting.
- **eMMC stem streaming**: read-ahead, underrun counters, and the actual
  `emmc_read_blocks`/`emmc_write_blocks` binding for `st_transfer.c`'s
  (already-implemented) transactional state machine.
- **The physical control scanner wired to `st_gesture.h`**: the ladder-band
  decode table, hysteresis, and debounce that turn raw ADC codes into the
  clean press/release edges `st_gesture_process_edge()` expects. The
  measured-band methodology is proven in `firmware/stemtape/src/main.c`
  and `firmware/src/main.c`; porting it here (plus the fader ADC reads
  driving `st_gesture_process_fader()`) is the next step, not invented
  ahead of the audio engine it would control.
- **USB companion transfer**: not enabled in this pass at all (see
  `prj.conf`) — USB is a maintenance-only connection and is brought up
  once there is a library to transfer songs into.
- Full verbatim parity with `docs/FIRMWARE_CONTRACT_V1.md`'s mute/solo/
  link/song-selection grammar (a 67-row contract built for a different,
  richer MIDI/Heads-mode system) — `st_gesture.c` implements a clearly-
  labeled, reasonable **subset** (tap = mute, hold past the contract's own
  700 ms threshold = solo); a verbatim port is future work.
- FX clear-all-latches uses a placeholder gesture pending the exact chord
  from the full contract.

No SP-1 hardware has been flashed or physically tested as part of this
release.
