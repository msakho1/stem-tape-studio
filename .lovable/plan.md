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

## 3. Three provenance-separated maps

**Correction:** `V26_ROWS_AS_REGISTRY` is not stock SP-1 behaviour. Its own source header states it is "transcribed verbatim from chattock.github.io/sp1-tape-looper (the v2.6 reference card)" (`src/machine/v26map.ts:1-14`). That is the **Tape Looper** documentation as imported into the web app — a mapping contract, not a record of untouched TE firmware. Nothing about stock SP-1 behaviour may be inferred from it, from row names, or from Tape Looper.

Three maps are maintained separately, each row carrying its own evidence field. A row without a citation is `unknown` and stays `unknown`.

### STOCK_SP1_MAP — evidence: original SP-1 guide, or observation of an untouched physical unit

Admissible evidence: TE's own printed/online guide for the SP-1, or a logged test session on a unit that has never been reflashed (control pressed → observed audio/LED result, recorded).

**Source S1 — stock firmware tutorial page** (`user-uploads://IMG_4259.jpeg` / `user-uploads://IMG_4259-2.jpeg`, hand-drawn three-column TE guide, "1 BASIC / 2 STEMS / 3 FX-FUN", TE logo bottom right). **Stock-guide-verified rows** below are read directly from S1; the guide is also publicly archived (URL supplied by user). All other S1/S2-derived details are **physical-verification-required** until tested on an untouched SP-1.

**Source S2 — Jay Gilligan, "Teenage Engineering Stem Player Deep Dive", `https://youtu.be/zynYy35AdE0` (2024-07-14, 1:18:16).** Documentary/observational only: the presenter reads from a TE-internal printed manual and demonstrates an unmodified unit. Use S2 for hypotheses, not for `stock-guide-verified` rows.

### STOCK_SP1_MAP — stock-guide-verified from S1

These rows are established by the TE guide itself. Precise timing, release behaviour, FX routing and simultaneous-hold interactions are **physical-verification-required**.

| Stock row | Gesture | Source |
|---|---|---|
| power | hold FUNCTION side button 5 s → ON/OFF | S1 column 1 |
| master volume | `− / +` | S1 column 1 |
| transport | tap PLAY → play/pause | S1 column 1 |
| skip track | rocker backward/forward → previous/next song | S1 column 1 |
| stem volume | faders 1–4, per stem | S1 column 2 |
| solo stem | hold TRACK 1–4 → solo that stem | S1 column 2 |
| tape effect | FUNCTION + rocker | S1 column 2 |
| sound effects | FUNCTION + TRACK 1 = Filter, TRACK 2 = Delay/Echo, TRACK 3 = Distortion, TRACK 4 = Gate/Shutter | S1 column 3 |
| loop | hold PLAY | S1 column 3 |

### STOCK_SP1_MAP — physical-verification-required (S2 hypotheses)

The video (S2) suggests a richer model, but it is not the printed guide and must be confirmed on hardware before it is treated as stock fact:

- Four firmware modes: basic / advanced / advanced + PO sync / advanced + MIDI clock, selected by FUNCTION + PLAY while stopped.
- In advanced mode the rocker becomes varispeed/reverse (not skip); `+/−` sets speed step; LEDs indicate step.
- Hold PLAY loop: S2 claims `+/−` sets loop division, rocker nudges loop position, short FUNCTION latches, PLAY unlatches.
- Track select: short FUNCTION click while playing, then a track button sets the active stem for FX.
- FX routing: hold FUNCTION + track button applies that effect to the **active stem**, with four variations per effect selected by `+/−`; one effect per stem, non-layering.
- Latch idiom: short FUNCTION while holding a control latches that state; all-four-LED blink confirms.
- LEDs: side = global VU / battery when paused; front = per-stem VU, active-stem solid, loop/speed/variation state.
- Bluetooth: `+` and `−` together enters pairing.
- Sync out / MIDI clock in modes 3 and 4.
- Auto power-off after idle.
- Ships with *Jesus Is King* stems; no documented user audio loading.

### Still `unknown` / do not infer

- exact loop length and whether release always ends the loop or latches
- whether the guide's "solo" is momentary or latching
- whether FX are per-stem, master, or active-stem-only in advanced mode
- heads/multi-playhead behaviour (no stock evidence; treat as Stem Tape invention)
- storage layout, song count, USB data protocol
- exact power-hold duration (S1 says 5 s, S2 says "a couple of seconds")
- MIDI-clock mode behaviour
- LED brightness/bank semantics beyond the guide

### Consequences for the port

(a) The nine stock-guide-verified rows are the only behaviours that can be claimed as "retained from stock" without hardware proof. (b) S2's advanced-mode varispeed/reverse is a plausible stock antecedent for Stem Tape's rocker, but it is **not** verified by the guide and must be confirmed before the port claims it. (c) The Tape Looper v2.6 rows (37 rows, timing thresholds, multi-taps, Heads, scrubbing, per-stem bar loops, etc.) are **not** established by either the TE guide or the video; they remain Tape Looper / Stem Tape inventions. (d) Stock's apparent "one effect per stem" model conflicts with Stem Tape's 12-FX layering — an intentional departure to record.

Complete the physical-verification pass on the donor unit during Phase 0, **before** the first flash; that observation pass is the only chance to capture it.



### TAPE_LOOPER_MAP — evidence: named tag, file and line in `chattock/sp1-tape-looper`

Admissible evidence: a citation of the form `v2.7.1-official : firmware/src/main.c : L####`, read from the fork.

**Current status: cited only to the v2.6 reference card as transcribed at `src/machine/v26map.ts:26-79` (37 rows), not yet to firmware source.** Before any porting work, each row must be re-verified against `v2.7.1-official` source and re-cited to file and line; the reference card may lag the firmware. Rows whose behaviour cannot be found in source are marked `unknown`, not assumed.

Firmware-side capabilities to cite the same way (currently attributed to the repo's README and file list, not to line-level source): eMMC audio streaming (`firmware/src/sp1_emmc.c`), UAC2 output, ADC control scan (`firmware/app.overlay`), LED driver, transport, varispeed/rocker, per-track loop record/play, tap tempo, WebSerial transfer protocol, battery gauge.

### STEM_TAPE_MAP — evidence: current web-app mapping registry and engine code

This map is fully cited and is the behavioural specification for the port:

1. Four-stem playback + per-stem fader gain — `src/machine/stemTapeV1Map.ts` `STEM_ROWS` (L60-113).
2. Chord/deferred-tap arbiter, single/double/triple, thresholds 450/600/700/900 ms, `suppresses` sets — `src/machine/chordArbiter.ts`, `src/input/gestures.ts`, thresholds declared per row in `stemTapeV1Map.ts`.
3. Remapped rows `play.cue`, `rocker.scrub`, `rocker.chop.play` — `stemTapeV1Map.ts` `TRANSPORT_OVERRIDE_ROWS` (L115-170), each carrying its own `supersedes` and `originalBehaviour` fields.
4. Global scrub + frame-exact handoff — `src/audio/engine.ts:114-116` (`SCRUB_HANDOFF_MIN_S`, `SCRUB_HANDOFF_FADE_S = 4 ms`), landing ≤2 frames per `src/audio/__tests__/scrubHandoff.test.ts`.
5. Lane scrub, FUNCTION + fader, ceiling 1.5× — `src/audio/scrub.ts:37` (`HEAD_SCRUB_MAX_RATE`), row `heads.lane.scrub` (`stemTapeV1Map.ts:229-245`).
6. Per-lane one-bar loops with hidden-timeline rejoin at the next bar — `src/audio/engine.ts` (`relocateShared`, `relocateLane`, `scheduleLoopRelease`), `src/audio/__tests__/loopRejoin.test.ts`.
7. Heads v2: four playheads on one source, dual-bus isolation, 40 ms equal-power crossfade, entry-snapshot restore — `src/audio/headLanes.ts`, `src/audio/engine.ts`, `src/audio/__tests__/busIsolation.test.ts`, rows `HEADS_V2_ROWS` (L179-266).
8. Lane reverse, loop-only vs. free-running rejoin — `src/audio/engine.ts` (`respawnLane`), row `heads.lane.reverse` (L246-255).
9. Play/stop inertia envelopes — `src/audio/inertia.ts`.
10. Twelve FX as 4 banks × 3, signal order TONE→MOD→MOTION→SPACE — `src/machine/fx12.ts:68-112`, `src/audio/fx/banks.ts`.

### Retain / replace / remove, relative to stock

Every entry here is **provisional and unresolvable until STOCK_SP1_MAP is populated**, because the "replaces" column currently compares against Tape Looper v2.6, not stock. What the registry actually records:

- **Replaces (Tape Looper v2.6 rows, self-declared):** `play.restart` → `play.cue`; `rocker.chop` → `rocker.scrub` with chop moved to PLAY + rocker; `heads.source` → `heads.lane.play`; `heads.replay` → `heads.lane.latch`; `heads.scrub` → `heads.lane.scrub`. Each carries an `originalBehaviour` string in `stemTapeV1Map.ts`.
- **Retains:** the remaining v2.6 rows, marked `provenance: "v2.6"`.
- **Removes:** live-input recording and PRINT (removed in earlier phases), `track.delete`.
- **Adds with no v2.6 antecedent:** `provenance: "extension"` rows — four-stem selection/solo/link, FX overlay, grid rows, WAV export.
- **Against stock, from S1 only (stock-guide-verified):** *retains* power hold, volume, PLAY/PAUSE tap, per-stem fader volume, solo-by-hold, rocker skip, FUNCTION + rocker tape effect, FUNCTION + track effects (Filter · Delay/Echo · Distortion · Gate/Shutter), and hold-PLAY loop. *Replaces* nothing provably beyond the additions below, because the guide does not establish varispeed, reverse, active-track select, latch, or any other advanced-mode detail. *Adds with no stock antecedent* the 37 Tape Looper rows, timing thresholds, multi-tap/deferred arbitration, Heads mode, scrubbing, per-stem bar loops, hidden-timeline loop rejoin, 12-FX layering, user stem import, and WAV export. *Removes* the stock mode selector (1–4), PO/MIDI sync out, Bluetooth pairing, and battery-level LED reporting from the web app's scope only — on hardware these must survive the port untouched if present.

### Stays browser-only (transfer tool)

File import/decode, mono downmix, **BPM/beat-phase/bar detection** (`src/audio/gridAnalysis.ts` — ship the detected grid as metadata alongside the stems), waveform rendering, project/session store (OPFS/IndexedDB), WAV export, guide/illustration, `.stemtape` packaging.


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
- **Unknowns/blockers:** whether the Solderless updater can read firmware back at all (assume no); whether a hash-verified stock image exists; whether the bootloader survives a failed application write; eMMC capacity; codec part number; whether the stock bootloader enforces signatures; real sample rate/latency ceiling; whether the shared analogue sense line can resolve the multi-control chords Stem Tape requires (FUNCTION + Track + Volume is three simultaneous presses).
- **Hardware tests:** two WebSerial round trips (Phase 0), chord resolution matrix on the sense line, sustained 4-stream eMMC throughput, thermal/battery under load.

## 7. Go / no-go

**Conditional go.** The base is genuinely open (MIT, full Zephyr source), the SoC is documented, and the browser build is an exact behavioural spec. Three conditions gate it: two clean WebSerial round trips in Phase 0, a hash-verified stock image (or explicit acceptance that reversion to stock is unavailable), and evidence the analogue sense line can report the three-control chords the map depends on. Failing the last means Stem Tape ships as a reduced mapping, not the full registry.

**Smallest safe first experiment:** Phase 0a alone — connect one donor unit in firmware mode to the Solderless updater and document, with screenshots, exactly which operations it offers. Whether a read-back path exists decides whether the rest of Phase 0 is a reversible test or a one-way commitment. Nothing is flashed at this step.


**First implementation milestone:** phase 3 — unmodified Tape Looper plus a serial trace of the ordered command stream, so the hardware's control events can be diffed against the browser twin's before any behaviour is forked.
