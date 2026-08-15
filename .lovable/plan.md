# Forking Tape Looper into Stem Tape on real SP-1 hardware

## 1. Authoritative sources (verified by fetch, not inference)

| Item | URL | Kind | License |
|---|---|---|---|
| Tape Looper firmware | github.com/chattock/sp1-tape-looper | full C source (`firmware/src/main.c`, `sp1_emmc.c/.h`, `app.overlay`, `prj.conf`, `boards/`, `zephyr-patches/uac2-windows-fs-feedback.patch`) | MIT |
| Release `v2.7.1-official` | same repo, asset `sp1_looper.bin` (109,240 B, sha256 e1a9152b…) | prebuilt binary | MIT |
| Latest sampled commit | `11bc91799e877b344475ce583a2c005fa6f657f7` | source | MIT |
| Loop transfer tool | chattock.github.io/sp1-tape-looper (source in `docs/`) | WebSerial source | MIT |
| Hardware/dev notes | github.com/timknapen/SP-1-dev (+ wiki, Discord) | documentation | MIT |
| Flasher / stem loader | solderless.engineering | hosted binary tool, source not located | unknown |
| Bootloader crack report | llllllll.co/t/te-stem-player/66795/709 | forum documentation | n/a |

Disambiguation: the 2021 Kano "Donda Stem Player" (RE'd by `krystalgamer/stem-player-emulator`, web-app only) is a **different device**. All firmware work targets TE's unreleased SP-1. Stock TE SP-1 firmware is **closed source** — no merge is possible; Tape Looper is the only legal base.

## 2. Hardware audit

- SoC: Nordic **nRF52840**, Cortex-M4F @64 MHz, **256 KB RAM / 1 MB flash** on-chip.
- Bulk storage: external **eMMC** (driver `sp1_emmc.c`); capacity UNKNOWN.
- Audio: discrete codec, software-configured (firmware references codec init and a −19 dB pad); part number UNKNOWN.
- Controls: 4 Track buttons, FUNCTION, PLAY — all five on **one shared analogue sense line** — 4 faders, 1 rocker; all read via ADC (`app.overlay`).
- LEDs: per-track + status, dimmable, used as battery gauge and metronome.
- USB: **UAC2** audio device + serial port (WebSerial transport). No mass storage.
- Bootloader: TE stock, community-bypassed; entry = hold Track 1 + Track 4 while plugging USB-C. Image format = raw unsigned `.bin`.
- Recovery: **no documented unbrick path** — the single largest risk.

## 3. Port matrix (Stem Tape behaviour = source of truth)

**A. Already in Tape Looper:** eMMC audio streaming, UAC2 out, ADC control scan, LED driver, transport, varispeed/rocker, per-track loop record/play, tap tempo, WebSerial transfer protocol, battery gauge.

**B. Stock SP-1 behaviour to reproduce:** the 37 `V26_ROWS_AS_REGISTRY` rows in `src/machine/stemTapeV1Map.ts` — these are already Tape Looper v2.6 semantics, so mostly "don't break".

**C. Stem Tape additions to port (C firmware):**
1. Four-stem simultaneous playback + per-stem fader gain (`STEM_ROWS`).
2. Chord/gesture arbiter: `src/machine/chordArbiter.ts`, `src/input/gestures.ts` — deferred tap arbitration (single/double/triple), 450/600/700/900 ms thresholds, `suppresses` sets.
3. Remapped rows: `play.cue`, `rocker.chop.play`, `rocker.scrub`.
4. Global scrub + frame-exact handoff (`SCRUB_HANDOFF_FADE_S = 4 ms`, landing ≤2 frames).
5. Lane scrub via FUNCTION + fader, capped **1.5×** (`HEAD_SCRUB_MAX_RATE`).
6. Per-lane one-bar loops with **hidden-timeline rejoin** at the next bar (`relocateShared`/`relocateLane`, `scheduleLoopRelease`).
7. Heads mode v2: four playheads on one source, dual-bus isolation (normal bus gated to 0, 40 ms equal-power crossfade), entry snapshot restore.
8. Lane reverse (loop-only vs. free-running rejoin).
9. Inertia envelopes on play/stop (`src/audio/inertia.ts`).
10. FX: 4 banks × 3 (`src/machine/fx12.ts`), signal order TONE→MOD→MOTION→SPACE.

**D. Stays browser-only (transfer tool):** file import/decode, mono downmix, **BPM/beat-phase/bar detection** (`gridAnalysis.ts` — ship the detected grid as metadata alongside the stems), waveform rendering, project/session store (OPFS/IndexedDB), WAV export, the guide/illustration, `.stemtape` packaging.

## 4. Fork structure

Fork `chattock/sp1-tape-looper` → `stem-tape-fw`, keep upstream as a remote and rebase onto tags. Never edit `sp1_emmc.c`, `boards/`, or the UAC2 patch in feature commits. New code in `firmware/src/stemtape/`: `gestures.c`, `arbiter.c`, `map_table.c` (generated from `stemTapeV1Map.ts` via `exportMapJson()`), `heads.c`, `scrub.c`, `fx/`. Likely modified upstream files: `main.c`, `prj.conf`, `app.overlay`, LED/ADC scan.

Build: Zephyr **v4.3.1**, SDK **0.17.4**, apply the UAC2 patch, `west build -p -b stem_player firmware -- -DBOARD_ROOT=$(pwd)`.

## 5. Phased bring-up (each step a reversible checkpoint = archived `.bin` + tag)

No step opens the enclosure. No SWD, no JTAG, no soldering. Every flash and every recovery goes through the Solderless WebSerial updater (power off → hold Track 1 + Track 4 while plugging USB-C → hold until the Track 1 LED lights → Connect, desktop Chrome/Edge only).

0. **Phase 0 — WebSerial-only recovery proof.**
   - **0a. Establish what the updater can actually do.** Open solderless.engineering in Chrome, connect a donor unit in firmware mode, and enumerate every control the page exposes. Record whether a read/export/dump path exists at all. *Current status: no evidence found that the tool can export installed firmware — the only documented flow is writing a supplied `.bin`. Until 0a proves otherwise, treat it as **flash-only** and never call it a firmware backup.*
   - **0b. Back up user data first.** Export all stems, loops and settings off the device via the existing transfer tool (chattock.github.io/sp1-tape-looper) and archive them with hashes. This is a data backup, not a firmware backup.
   - **0c. Obtain a verified stock image.** Source a stock TE SP-1 firmware `.bin`, record its sha256, and corroborate the same hash from at least two independent community sources. *No publicly distributed stock image with a published hash was found in research — see the accepted risk below.*
   - **0d. Flash unmodified Tape Looper `v2.7.1-official`** (`sp1_looper.bin`, 109,240 B, sha256 e1a9152b…) via Solderless.
   - **0e. Re-enter firmware mode and restore stock** through Solderless using the 0c image.
   - **0f. Repeat 0d–0e a second time.** After each of the four flashes, verify: audio out (UAC2 enumeration + audible playback on both channels), all 11 controls via ADC response, every LED including the battery gauge, and USB re-enumeration as both audio device and serial port.
   - **Exit criterion:** two complete Tape-Looper → stock round trips with all four subsystems green after every flash.
   - **Accepted risk, explicit:** (i) if 0c cannot produce a hash-verified stock image, the round trip cannot be closed and the only reversion target is Tape Looper itself — the device would be permanently off stock firmware; (ii) if a flash fails mid-write and the bootloader does not survive it, there is no WebSerial recovery and, with the enclosure closed, no recovery at all. Community reports confirm the bootloader is normally reachable over USB, but nothing found confirms it survives a corrupted application write. Both risks are accepted knowingly or Phase 0 is a no-go.
1. Build **unmodified** `v2.7.1-official` from source, flash it over WebSerial, and verify behaviour matches the released `.bin`.

2. Donor hardware verification: ADC scan of all 11 controls, LED sweep, UAC2 enumeration, eMMC read/write, battery gauge.
3. Instrumentation only: emit a serial command stream mirroring Stem Tape's ordered commands. No behaviour change.
4. Gesture arbiter + generated map table (C-group 2/3).
5. Four-stem playback + faders (C1).
6. Varispeed/inertia, global scrub, lane scrub (C4, C5, C9).
7. Lane loops + hidden-timeline rejoin, reverse (C6, C8).
8. Heads mode + bus isolation (C7).
9. FX banks, cheapest first; drop `heavy: true` algorithms (Scatter, Shimmer, Freeze) if the budget fails.
10. Transfer-tool protocol extension: stems + grid metadata + mappings.

## 6. Budgets, risks, unknowns

- **CPU:** 64 MHz M4F, no FPU-heavy headroom. Four streams at 44.1 k with resampling ≈ the whole budget; FX and Heads (4 extra voices on one source) are the risk. Measure cycles per block at step 5 before promising steps 8–9.
- **RAM:** 256 KB total. No decoded-buffer model — everything must stream from eMMC with small ring buffers. Heads = 4 extra read cursors × ring buffer; size them in step 8 or cut head count.
- **Flash:** 1 MB; Tape Looper already uses ~109 KB. Map table and FX coefficients must live in flash, not RAM.
- **Licensing:** upstream MIT — fork freely with attribution. Stock TE firmware must never be copied or merged. Solderless flasher licensing is unknown; do not redistribute it.
- **Unknowns/blockers:** no confirmed unbrick path; eMMC capacity; codec part number; whether the stock bootloader enforces signatures; real sample rate/latency ceiling; whether the shared analogue sense line can resolve the multi-control chords Stem Tape requires (FUNCTION + Track + Volume is three simultaneous presses).
- **Hardware tests:** SWD recovery ×2, chord resolution matrix on the sense line, sustained 4-stream eMMC throughput, thermal/battery under load.

## 7. Go / no-go

**Conditional go.** The base is genuinely open (MIT, full Zephyr source), the SoC is documented, and the browser build is an exact behavioural spec. Two conditions gate it: a proven SWD recovery path, and evidence the analogue sense line can report the three-control chords the map depends on. Either failing means Stem Tape ships as a reduced mapping, not the full registry.

**Smallest safe first experiment:** on one donor unit, wire SWD, dump and restore stock firmware, then build and flash unmodified Tape Looper `v2.7.1-official` from source and recover back to stock. Success = the device survives two full round trips. Nothing Stem Tape-specific is written until that passes.

**First implementation milestone:** phase 3 — unmodified Tape Looper plus a serial trace of the ordered command stream, so the hardware's control events can be diffed against the browser twin's before any behaviour is forked.
