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

**Source S1 — stock firmware tutorial page** (`user-uploads://IMG_4259.jpeg`, hand-drawn three-column TE guide, "1 BASIC / 2 STEMS / 3 FX-FUN", TE logo bottom right). Documentary only.

**Source S2 — Jay Gilligan, "Teenage Engineering Stem Player Deep Dive", `https://youtu.be/zynYy35AdE0` (2024-07-14, 1:18:16).** Strongest available stock evidence: the presenter reads aloud, line by line, from the TE-internal printed instruction manual anonymously mailed to him (S2 @18:29–19:29), while operating an unmodified unit on camera. Every row below is *manual text + observed hardware behaviour*, cited by transcript timestamp. Companion community record: `https://llllllll.co/t/te-stem-player/66795`.

**Device model (S2).** Four firmware modes selected by holding FUNCTION then PLAY (function slightly first is more reliable) while stopped; four LEDs flash, `+/−` or the track buttons choose the mode, PLAY confirms (@31:48, @39:18, @1:04:45):

1. basic · 2. advanced · 3. advanced + pocket-operator sync out · 4. advanced + MIDI clock out

| Stock row | Behaviour | Mode | Evidence |
|---|---|---|---|
| power | hold FUNCTION/side button ~2–5 s → ON/OFF, LED sweep | both | S1; S2 @26:32, @28:43 |
| transport | PLAY tap → play/pause, with an audible **tape start/stop ramp** on both edges | both | S2 @30:56 |
| volume | `+ / −` | both | S2 @29:44 |
| track volume | four faders, per stem | both | S2 @33:15 |
| solo/mute stem | track button solos/mutes; **functions stack** — multiple solos and other actions combine freely | both | S2 @35:28, @36:14 |
| skip track | rocker FWD/REV = next/previous song (and restart-current) | basic | S1; S2 @33:57 |
| skip track | FUNCTION + rocker = next/previous song | advanced | S2 @49:59 |
| slow motion | FUNCTION + PLAY toggles a single fixed slow playback | basic | S2 @34:57 |
| varispeed | rocker forward = faster, rocker back = **reverse playback**; `+/−` chooses the speed step; LEDs show the step; rocker again exits | advanced | S2 @50:34–@53:12 |
| loop | **hold PLAY** → one-bar loop, side + front LEDs flash; release ends it | basic | S2 @34:24 |
| loop | hold PLAY → loop; while held, `+/−` sets the **loop division**, rocker **nudges the loop position** forward/back through the song; release disables; short FUNCTION while held **latches** it; PLAY unlatches | advanced | S2 @47:15–@49:37 |
| loop divider preset | when not looping, `+/−` presets the loop divider | advanced | S2 @47:47 |
| track select | short FUNCTION click → track-select (all four LEDs light); press a track button to set the **active stem**; only works while playing; selected stem shows a solid LED, the others show per-stem VU | advanced | S2 @39:18–@41:35 |
| effects | hold FUNCTION + a track button = apply that button's effect **to the active stem** (not to the button's own stem): 1 Filter · 2 Chorus/Delay · 3 Distortion · 4 Gate | advanced | S1; S2 @42:15–@43:21 |
| effect variations | while holding the effect button, `+/−` cycles **four variations** per effect; for Chorus/Delay, variations 1–2 chorus, 3–4 delay; LEDs show the variation | advanced | S2 @43:50, @46:42 |
| effect latch | keep holding the track button, release FUNCTION, click FUNCTION → effect **locked** to the active stem; all four LEDs blink to confirm any latch | advanced | S2 @44:25, @52:27 |
| effect exclusivity | **one effect per stem** — a second effect overwrites, it does not layer | advanced | S2 @46:12 |
| effects persistence | latched effects survive song changes | advanced | S2 @49:59 |
| clear effects | short FUNCTION while holding the latched effect's track button clears that stem; holding **all four** track buttons + FUNCTION clears everything; FUNCTION while soloing one track also clears | advanced | S2 @53:12–@54:12 |
| latch (general) | short FUNCTION press while holding track / PLAY / rocker latches that state (effect, loop, varispeed) | advanced | S2 @53:12 |
| LEDs | side LEDs = global VU while playing, **battery level when paused**; front LEDs = per-stem VU, active-stem solid, loop/speed/variation indication, all-four blink = latch confirmed | both | S2 @40:25, @41:35, @54:12 |
| bluetooth | `+` and `−` pressed together (device on) → BT pairing/search, LED indicates search | both | S2 @1:02:16 |
| sync out | mode 3 emits PO sync on the 3.5 mm sync jack, tracking the song BPM across track changes; mode 4 emits MIDI clock (untested on camera) | 3 / 4 | S2 @1:04:45, @1:06:13 |
| idle | auto power-off after a few minutes when not playing | both | S2 @54:42 |
| content | ships with the *Jesus Is King* stems only; **no user-facing way to load your own audio** on stock | — | S2 @10:47 |
| I/O | 3.5 mm line/headphone out, PO/MIDI sync jack, USB-C (charging), internal speaker, Bluetooth audio out | — | S2 @21:53, @22:22 |

Still `unknown` after S1+S2 — do not infer:

- whether "sound effects" are per-stem only or ever master (S2 shows per-active-stem in advanced; basic-mode gate is applied per track button, @36:39)
- heads/multi-playhead behaviour (no evidence it exists; treat as a Stem Tape invention)
- storage layout, song count, USB data protocol (USB-C is described as charging only)
- exact power-hold duration (S1 says 5 s, S2 says "a couple of seconds")
- MIDI-clock mode behaviour (mode 4 never demonstrated)
- LED brightness/bank semantics beyond the states listed above

Consequences for the port: (a) stock **does** have varispeed *and* reverse on the rocker in advanced mode, so Stem Tape's rocker is closer to stock than S1 alone suggested — the departure is scrub/chop, not speed; (b) stock's latch model (short FUNCTION click while holding a control) is the native idiom for every sticky state and should be preserved rather than replaced by Stem Tape's own latching gestures where they conflict; (c) stock enforces one effect per stem, which the 12-FX bank design breaks — an intentional departure to record; (d) FUNCTION-click track-select with all-LED indication is the stock antecedent of Stem Tape's `lastTargetedTrack`.

S1/S2 are documentary. Confirm them row by row on the donor unit during Phase 0, **before** the first flash, and fill the unknown list; that observation pass is the only chance to capture it.



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
- **Against stock, from S1 + S2 (documentary, unconfirmed on the donor unit):** *retains* power hold, volume, PLAY/PAUSE tap with tape start/stop ramp, per-stem fader volume, solo/mute stacking, hold-PLAY one-bar loop with `+/−` division and rocker nudge, rocker varispeed with reverse, FUNCTION-click active-track selection, and a hold-engaged FX set; *replaces* the rocker's scrub/chop overlay (stock rocker = skip in basic, varispeed in advanced — Stem Tape adds scrub and chop), the four-effect model (stock: 4 effects × 4 variations, **one per stem, non-layering** → Stem Tape: 12 FX in four banks that do layer), and the latch idiom (stock latches everything with a short FUNCTION click; Stem Tape uses per-row gestures); *adds with no stock antecedent* Heads mode, per-lane reverse, hidden-timeline loop rejoin, user stem import, WAV export; *removes* the stock mode selector (1–4), PO/MIDI sync out, Bluetooth pairing, and battery-level LED reporting from the web app's scope only — on hardware these must survive the port untouched.

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
