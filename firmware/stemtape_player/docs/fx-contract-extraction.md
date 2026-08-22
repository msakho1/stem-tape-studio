# FX contract extraction — the state table, and the measured limit

Commit A. **No production audio path changed.** This is the extraction, the
state machine, and the resource arithmetic that has to exist before any of it
is wired.

Sources, in the order they win when they disagree:

1. `src/machine/chordArbiter.ts` — the running production arbiter
2. `src/machine/fx12.ts`, `src/audio/fx/banks.ts`, `src/machine/stemPerformance.ts`
3. `docs/firmware-contract-v1.json`, `docs/FIRMWARE_CONTRACT_V1.md`

---

## 1. Version skew, resolved rather than guessed

The `fx-overlay` layer in `docs/firmware-contract-v1.json` describes the **v3**
three-family model (`fx.filter.*`, `fx.echo.*`, `fx.reverb.*`, `track-button-4`
unused). `fx12.ts` is **v4** (`FX12_SCHEMA_VERSION = 4`, line 161) and carries
an explicit forward migration (`migrateLegacyStemFx`, lines 260-279). v4 wins;
the JSON rows are the superseded model, not a competing one.

Two v3→v4 mappings that decide firmware behaviour:

| v3 row | v4 meaning | Evidence |
|---|---|---|
| `fx.*.variation` — "±1 (4 presets)" | the **macro** (0..1, step 0.05) | `stemPerformance.ts:137-138` — *"Macro 0..1 drives the legacy 1..4 preset table with no second selector"* |
| `fx.*.momentary` on buttons 1-3 | bank momentary on buttons 1-**4** | `fx12.ts:82` MOD `buttonIndex: 3`; `fx12.ts:127-128` `RHYTHM_PHYSICAL_BUTTON = 4` |

`LEGACY_FAMILY_TO_BANK` (`fx12.ts:243-250`) confirms the bank indices:
filter→0 (TONE), beatRepeat→1/alg2 (MOD/Gate), echo→2 (MOTION), reverb→3
(SPACE). `BANK_FAMILY = ["filter", null, "echo", "reverb"]`
(`stemPerformance.ts:123`) agrees, with MOD deliberately having no legacy
family.

### The one genuine contradiction

The Volume−+Volume+ chord arrival window:

| Source | Value |
|---|---|
| `docs/firmware-contract-v1.json` → `timing.fxOverlaySecondPressMs` | **120 ms** |
| `docs/FIRMWARE_CONTRACT_V1.md:46` | **120 ms** |
| `src/machine/stemTapeV1Map.ts:274` (row text) | **120 ms** |
| `src/machine/chordArbiter.ts:52` `modifierArrivalMs`, applied at `:489` | **400 ms** |

No 120 ms constant exists anywhere in the arbiter. **Resolved to 120 ms** by
product decision. Firmware rationale, recorded because it also happens to be
the better engineering answer: the window is dead latency on *every ordinary
volume press*, because the SP-1 scan loop must withhold the individual action
until the chord can be ruled out. 120 ms is imperceptible; 400 ms is not, and
would also swallow two deliberate volume steps pressed 300 ms apart.

`ST_FX_CHORD_ARRIVAL_MS = 120`.

---

## 2. Control routing table

`FN` = FUNCTION. "claims" = the input is consumed and no normal-mode handler
may also process it.

| Input | Normal mode | FX mode | Evidence |
|---|---|---|---|
| Vol− | master volume − | see below | |
| Vol+ | master volume + | see below | |
| Vol− + Vol+, arrival ≤120 ms, both released <600 ms | **enter FX mode**, no volume change | **exit FX mode**, no volume change | `chordArbiter.ts:495-506` |
| Vol− + Vol+, overlap ≥2000 ms | pairing | pairing (suppresses `fx.overlay`) | `:493-494` |
| Vol− + Vol+, overlap 600–2000 ms | no-op, diagnostics only | no-op | `:507-513` |
| Vol− + Vol+, arrival >120 ms | not a chord; individual actions run | same | `:489-491` |
| FN + Vol− / FN + Vol+ (chord open) | grid quantise / tap tempo | **walk the FX target stem** (−/+) | `:323-335` |
| Track *n* alone | track solo/chord | **bank select + momentary ON**; release = momentary OFF | `:296-306`, `:588-596` |
| Track *n* then FN | — | **latch toggle** for that bank | `:428` |
| FN then Track *n* | FN+Track lane row | **not an FX intent** — belongs to the lane layer | `:288-295` |
| FN + all four Track | — | **clear all latches** | `:422` |
| Vol± **tap** (bank selected) | volume | **cycle algorithm ±1** | `:569-585` |
| Vol± **hold ≥450 ms** (bank selected) | volume repeat | **macro ±0.05, repeating every 120 ms** | `:336-341`, `:257-269` |
| PLAY | transport / loop | **unchanged** — Track buttons belong to FX even while PLAY is held | `:282-286` |
| Rocker | FN+rocker scrub, PLAY+rocker chop | **unchanged — the contract assigns the rocker no FX role** | only rocker rows are `tape/rocker.scrub`, `tape/rocker.chop.play` |

**The rocker carries no FX function.** Algorithm and macro both live on
Volume ±, discriminated tap vs hold. This is a finding, not an omission.

### Conflict priority

1. Pairing (≥2000 ms volume chord) — suppresses `fx.overlay` and `master.gain`
2. Volume chord (≤120 ms arrival) — claims both buttons *before* dispatch, so
   no master-volume step can leak
3. FN-first + Track — lane layer, FX must not claim
4. FX mode Track/Volume rows — claim on press
5. PLAY, loop, transport — never claimed by FX
6. Normal mode

---

## 3. FX-mode toggle state machine

States: `IDLE → ONE_DOWN → CHORD_PENDING → CHORD_ARMED → WAIT_RELEASE_ALL`.

| State | Event | Next | Action |
|---|---|---|---|
| IDLE | one Vol down | ONE_DOWN | start 120 ms arrival timer; **withhold** the individual action |
| ONE_DOWN | other Vol down ≤120 ms | CHORD_ARMED | claim both; cancel the withheld action |
| ONE_DOWN | 120 ms expires | CHORD_PENDING | dispatch the withheld individual Vol action now |
| ONE_DOWN | that Vol released | IDLE | dispatch the individual action (tap) |
| CHORD_PENDING | other Vol down | CHORD_PENDING | ordinary individual action; no chord |
| CHORD_ARMED | first release, overlap <600 ms | WAIT_RELEASE_ALL | **emit exactly one `FX_MODE_TOGGLE`** |
| CHORD_ARMED | first release, 600–2000 ms | WAIT_RELEASE_ALL | no-op + diagnostic |
| CHORD_ARMED | overlap reaches 2000 ms | WAIT_RELEASE_ALL | pairing |
| WAIT_RELEASE_ALL | both released | IDLE | — |
| WAIT_RELEASE_ALL | any Vol re-press | WAIT_RELEASE_ALL | ignored (one toggle per chord) |

`WAIT_RELEASE_ALL` is what makes a held chord emit exactly one toggle and makes
contact bounce inert: after the toggle, nothing fires until **both** buttons
have been seen released.

The nine required cases map onto it: Vol−→Vol+ and Vol+→Vol− are symmetric
(`ONE_DOWN` doesn't care which); same-scan detection enters `CHORD_ARMED`
directly with arrival 0; Vol− alone and Vol+ alone leave via the ONE_DOWN
timeout/release edges; held chord parks in `WAIT_RELEASE_ALL`; bounce is
absorbed there; releasing one before the other is the normal toggle edge; rapid
off/on works because `IDLE` is re-entered as soon as both are up.

### Mode-state invariants

FX mode is off after every boot; entering/exiting changes no volume, does not
touch the transport, seek, speed, loop window, BPM, beat phase or loop phase,
and does not reset delay lines, tails, selected algorithms, macros or latches.
Exiting leaves latched banks sounding. Momentary banks end on button release.
No FUNCTION, PLAY or Track press is needed to enter or exit.

---

## 4. Banks, in signal order

`BANKS[]` is declared in **signal** order; `buttonIndex` carries the physical
mapping, so `BANKS[i].buttonIndex != i` for MOD and MOTION (`fx12.ts:64-67`).

| Signal idx | Bank | Button | Algorithms (0,1,2) | Default macros |
|---|---|---|---|---|
| 0 | TONE | Track 1 | Filter, Exciter, Dirt/Crusher | 0.50, 0.40, 0.35 |
| 1 | MOD | Track 4 (RHYTHM) | Reel Flange, Formant Shift, Rhythmic Gate | 0.45, 0.50, 0.50 |
| 2 | MOTION | Track 2 | Tempo Echo, Pitch Echo, Granular Scatter | 0.50, 0.50, 0.40 |
| 3 | SPACE | Track 3 | Reverb, Shimmer, Spectral Freeze | 0.45, 0.45, 0.50 |

Signal chain: `source → TONE → MOD → MOTION → SPACE → fader → solo → master`.

Cycling an inactive bank changes what the next activation runs and does **not**
activate it (`fx12.ts:193-200`). A rejection marks one algorithm, never the
bank (`fx12.ts:214-218`, `banks.ts:19`).

---

## 5. THE MEASURED LIMIT

Computed by `.github/scripts/stemtape_fx_budget.py` — run it; every number
below is its output, not a summary of one.

Delay memory is set by musical time: a tempo-locked line of *D* beats needs
`D × sample_rate × 60 / bpm` frames, so the **slowest tempo the firmware
admits** sizes every echo. No BPM clamp exists today (`st_beat_phase.c:9-30`
accepts any nonzero `bpm_q8`), so `MIN_BPM` is a decision this analysis has to
surface. Reference song is 93.71 BPM, so `MIN_BPM = 70` is the honest floor.

One rack, mono delay storage, reference algorithm parameters:

| Bank | Arena | Worst member |
|---|---:|---|
| TONE | 96 B | Exciter |
| MOD | 1,024 B | Reel Flange |
| MOTION | **92,668 B** | Pitch Echo (1.125 beat @ 70 BPM) |
| SPACE | **41,440 B** | Spectral Freeze (430 ms) |
| **one rack** | **135,228 B** | |

Against **67,618 B free today** and **~110,000 B projected** after the unified
sector cache.

| Configuration | Mono | Stereo |
|---|---:|---:|
| 1 rack | 135,228 B | 270,040 B |
| 2 racks (one stem + global) | 270,456 B | 540,080 B |
| **5 racks (4 stems + global) — the full contract** | **676,140 B** | **1,350,200 B** |

The part has 262,144 B of RAM **in total**.

### What restriction buys

| MIN_BPM | max echo beat | freeze | scatter line | 1 rack | ×5 |
|---:|---:|---:|---:|---:|---:|
| 70 | 0.75 | 0.43 s | 0.10 s | 135,228 | 676,140 |
| 70 | 0.375 | 0.43 s | 0.10 s | 88,942 | 444,710 |
| 70 | 0.25 | 0.20 s | 0.06 s | **61,594** | 307,970 |
| 100 | 0.25 | 0.20 s | 0.06 s | 53,840 | 269,200 |
| 120 | 0.125 | 0.12 s | 0.04 s | 46,160 | 230,800 |

**One rack fits. Five racks do not fit at any setting.** The most aggressive
row — which needs MIN_BPM 120, already disqualified by the 93.71 BPM reference
song — still needs 230,800 B for five racks.

The binding constraint is not any single algorithm. It is that the contract
permits **five independent racks latched simultaneously**, and delay memory
cannot be shared between racks that are allowed to sound at the same time.
Section 11's arena sharing works *within* a bank (three mutually exclusive
algorithms) and that saving is already counted above.

---

## 6. Fixed-point formats

| Quantity | Format | Range |
|---|---|---|
| Audio sample, delay storage | Q15 `int16_t` | ±1.0 |
| Audio accumulator | Q15 in `int32_t`, saturating | ±65535.0 pre-clamp |
| Macro | `uint8_t` 0..20 (step 0.05 exactly) | 0.0–1.0 |
| Wet/dry gain, seam gain | Q8 `uint16_t`, unity 256 | reuses `st_seam.h` |
| Filter coefficients | Q14 `int32_t` | ±2.0 |
| Feedback gain | Q15 `uint16_t` | echo ≤0.72, reverb ≤0.9, freeze ≤0.82 |
| LFO phase | Q32 `uint32_t` accumulator | wraps naturally |
| Engage ramp | 576 samples, exact | `FX_ENGAGE_S 0.012 × 48000` |

Macro as `0..20` makes the 0.05 step exact and removes every rounding question
from the reference-vector comparison.

---

## 7. Not yet built, and why

Cross-language reference vectors and the DSP itself are **not** in this commit.
Both are shaped by how many racks exist and what the delay bounds are, and both
are wasted work if the answer to §5 changes either. The vector generator is
written against the frozen TypeScript once the rack count is fixed.

**Status: implemented — nothing. Production-linked — nothing. CI-proven — the
budget script runs. Physically verified — nothing.**
