# Stem Tape Player — standalone firmware

The standalone, four-stem Stem Tape player: a dedicated Zephyr app target
(separate from `firmware/stemtape`, the M0 MIDI control-surface
diagnostic, and `firmware/`, the SP-1 Tape Looper) that boots the board on
its own, with USB entirely optional.

It plays. A song uploaded over the companion protocol survives a power
cycle and streams from eMMC as four synchronized 24-bit/48 kHz stems —
confirmed on real SP-1 hardware, with the exact commit and BIN hash frozen
in [`docs/stem-tape-hardware-audio-baseline-1.md`](../../docs/stem-tape-hardware-audio-baseline-1.md).
"Deferred beyond this release" at the bottom is the honest boundary of
what that does and does not include.

## What this target delivers (real, built, tested)

- **Watchdog and power button hold-to-off** — reused verbatim from the
  pinned Tape Looper (`firmware/src/main.c`, commit `a8dd127`) and the M0
  target, checked before any other subsystem so recovery works even if
  something later hangs. The Tape Looper's in-firmware Track1+Track4 DFU
  combo is **removed** (product ruling): Track 1 and Track 4 are ordinary
  performance controls on this instrument, and a gesture that reset the
  device out of the running firmware mid-song was never compatible with
  playing it. Recovery is unaffected — see "Entering the UF2 bootloader"
  below.
- **The stored four-stem playback engine** — `streamer_thread()` reads the
  selected song's sectors from eMMC into a lock-free SPSC ring
  (`src/st_stem_bufmbox.c`) with eleven sectors of read-ahead;
  `audio_thread()` decodes and mixes them (`src/st_sector_v11.c`,
  `src/st_stem_stream.c`, `src/st_stem_mix.c`) into the SP-1's existing
  I2S output. The Stem Tape dispatch is a genuine **bypass**: when a stem
  song is playing, none of the inherited classic recorder/transport/
  resampler work executes at all. The song's own STIX record
  (`src/st_stix.c`) is the sole authority for geometry and tempo.
- **The eight-LED semantic owner** (`src/st_led_mvp.h`/`.c`) — one pure,
  host-tested function that turns real runtime state into one complete
  eight-LED frame, rendered through main.c's **existing TIMER3/GPIO
  soft-PWM driver** inherited from the Tape Looper. `led_service()` gathers
  live transport/mixer/charger state and does nothing else; every decision
  lives in `st_led_mvp_decide()`.

  Each LED carries a real 0..255 brightness (the renderer gives every pin
  its own sigma-delta duty), which is what the playing animation needs: a
  shared beat envelope derived from the selected song's own STIX tempo,
  scaled per stem by that stem's output activity, with a 1→2→3→4 chase
  accent marking bar position and a tempo pulse on S4. The beat phase is
  re-derived from the authoritative `song_frame` on every call
  (`st_beat_pulse()`), so there is exactly one clock — the audio path's —
  and no LED timer that can drift against it. A Track button held is an
  immediate momentary solo.

  The side row has exactly two states, chosen by transport alone: **playing**
  gives S4 the beat envelope and leaves S1–S3 dark; **not playing** shows the
  four-step battery gauge, continuously — with no song selected, with a song
  selected but stopped, on battery, on USB, charging, full, mid-transfer, or
  with a Track button held. The only thing that darkens it while stopped is a
  reading the gauge itself refuses to trust (unavailable, faulted, or never
  seeded), which is left dark rather than rendered as a fabricated level.

  **Correction.** This section previously claimed the eight-LED PWM
  renderer (`led_duty.c`, `led_render.c`, `led_render_policy.c`) was
  driving a semantic pattern engine (`src/st_led_pattern.c`) on this
  device. That was false, and it was false in a way that mattered: none of
  those four files are in this target's `CMakeLists.txt`, so none of them
  ever executed here. The LEDs were actually driven by inherited Tape
  Looper code — a 16-song bank/position display on the side row that made
  a side LED blink permanently on a one-song device, plus a standby chase
  and an ad-hoc peak meter on the track row. `st_led_mvp.c` replaces all
  three with a single owner; the M0 modules remain M0's, still host-tested,
  still not compiled into this firmware.
- **The versioned companion transfer protocol** — the transactional
  begin/stage/verify/commit/abort state machine the companion app uploads
  through, host-tested and running on the device. The **current,
  authoritative** contract is v1.1 (`src/st_stix.c`, `src/st_stcp.c`,
  `src/st_sector_v11.c`, `src/st_ab_session.c`, `src/st_bulk_xfer.c`) —
  see [`docs/stem-tape-transfer-v1.1.md`](../../docs/stem-tape-transfer-v1.1.md).
  The earlier v1.0 line (`src/st_transfer.c`, `src/st_storage_layout.c`,
  `src/st_sector_codec.c`, `src/st_library_io.c`, `src/st_xfer_wire.c`) is
  **retired and deliberately not linked**: `main.c` no longer contains any
  dispatch that reaches it, so no v1.0 write path exists in the ELF. Those
  files remain in the tree with their host tests as regression evidence
  only. See [`docs/stem-tape-transfer-v1.md`](../../docs/stem-tape-transfer-v1.md)
  for that superseded design.

The three bullets below describe **host-tested reference modules that are
NOT compiled into this firmware** — they are design work preserved for
features that have not landed, and nothing in them executes on the device:

- **The physical gesture grammar** (`src/st_gesture.c`) — idle debounce,
  chord atomicity, fader pickup/crossing, the global loop momentary/latch/
  release grammar, STEM/GLOBAL FX scope open/close/cycle, FX Track
  momentary/latch/unlatch, and the full forward/reverse scrub latch
  grammar (task section 7, items 1-10) — as a PURE, host-tested state
  machine that emits semantic commands. It is NOT what runs: the shipping
  control path is `src/st_ctl.c` + `src/st_ladder.c` + `src/st_loop.c`,
  with `main.c`'s inherited ladder decode still serving the no-stem-song
  case.
- **The four persistent scrub speeds and the tape-inertia release ramp**
  (`src/st_scrub.c`) — ported from `src/audio/inertia.ts`'s exact
  documented math (`GLOBAL_SCRUB_SPEEDS`, the finite exponential curve,
  the zero-crossing solver), not re-derived.
- **The FX bank/algorithm catalog** (`src/st_fx_catalog.h`) — ported
  verbatim from `src/machine/fx12.ts`'s twelve-algorithm model (Filter/
  Exciter/Dirt-Crusher in TONE, plus MOD/MOTION/SPACE), including the
  exact physical Track-button mapping. The DSP audio processing per
  algorithm is not ported — see "Deferred".

All of the above compiles as pure, portable C and is exercised on the host
with zero Zephyr/hardware dependency by the suites in `tests/` — the CI
run's own step summary is the authority on which suites ran and how many
checks each made, since those counts move with every change and a number
written here goes stale immediately.

Host tests are supporting evidence, not proof that the device works. What
they establish is that a pure function computes what it claims to; what
runs on the SP-1 is settled by `CMakeLists.txt`, the wiring gates in
`.github/scripts/`, and physical testing.

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

## Deferred beyond this release

**Correction.** This section previously described the target as "a
skeleton that boots and proves its foundations, not yet a playable
instrument," and listed the audio engine, eMMC stem streaming, the
physical control scanner and USB companion transfer as unimplemented. All
four have since landed and the list was not updated. It closed with "No
SP-1 hardware has been flashed or physically tested as part of this
release," which is also no longer true — see the baseline below. What
follows is the current, accurate division.

**Implemented and hardware-confirmed**: a song uploads over the companion
transfer protocol, survives a power cycle, and plays back continuously as
four synchronized 24-bit/48 kHz stems through the SP-1's own speaker and
headphone outputs, with the Track buttons soloing and the fader riding
master level. The frozen, byte-reproducible baseline for that is recorded
in [`docs/stem-tape-hardware-audio-baseline-1.md`](../../docs/stem-tape-hardware-audio-baseline-1.md).
Both codecs (CS42L42 headphone, TAS2505 speaker) are brought up by this
firmware's own init.

Still NOT implemented, and not claimed to be:

- **Per-algorithm FX DSP.** `src/st_fx_catalog.h` ports the twelve-
  algorithm selection/routing model; the Web Audio graphs in
  `src/audio/fx/banks.ts` that actually process audio are not ported. No
  FX processing runs on the device.
- **The scrub resampler.** `src/st_scrub.c` ports the speed table and the
  tape-inertia release ramp as pure math, host-tested — but it is **not
  linked into this target** and nothing calls it, so there is no
  variable-speed playback on the device.
- **`st_gesture.c` as the control grammar.** With a Stem Tape song
  selected the surface is owned by `src/st_ctl.c` — one ladder sample per
  pass through `src/st_ladder.c`'s measured classifier, then the Track
  mask, the PLAY gesture and `src/st_loop.c`'s window; `main.c`'s
  inherited ladder decode still runs the Tape Looper behaviour when no
  stem song is selected. `st_gesture.c`'s richer chord/loop/FX-scope state
  machine is host-tested but **not linked** — it is reference, not
  runtime. The same is true of `st_scrub.c` and `st_led_pattern.c`; see
  `CMakeLists.txt`, which is the authority on what is compiled in.
  `docs/stem-tape-control-v1.md` is the one-page description of what
  actually runs, and `docs/ladder-measured.json` is the hardware capture
  every band is derived from.
- **Headphone-insertion speaker muting.**
- **Recording and overdub.** This is a player. There is no record path.
- Full verbatim parity with `docs/FIRMWARE_CONTRACT_V1.md`'s mute/solo/
  link/song-selection grammar (a 67-row contract built for a different,
  richer MIDI/Heads-mode system). Stem Tape's own Track button is an
  immediate momentary solo — press to solo, release to restore — and has
  no persistent mute at all; that is a deliberate product decision, not a
  partial implementation of the contract's tap-to-mute.

**How to tell what is real.** Any module in this directory that is not
listed in `CMakeLists.txt`'s `target_sources()` is not in the firmware,
however thoroughly it is host-tested. That distinction has been got wrong
in this README twice; `CMakeLists.txt` is the only source of truth for it.
