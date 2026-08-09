# Universal Lane Controls, Automatic Grid, Recording + PRINT Removal

Consolidated revision. Supersedes the earlier Track/Grid/Recording plan.

## 0. Verified current state (read this session)

- `heads.print` is live in five places: `src/machine/v26map.ts:70`, `src/machine/stemTapeV1Map.ts:381`, `src/machine/surface.ts:621-622`, `src/audio/commands.ts:65`, `src/audio/engine.ts:939` and `:3199`.
- `src/audio/print.ts` has exactly **one** importer: `src/audio/useAudioEngine.ts:5` (`installPrintCommit`). Nothing retained imports it, so it is removable — but it imports `./wav` and `./ingest`, which are retained.
- Displaced Function + Fader mappings are `fader.window` (FN+1/2/3), `fader.windowReverse` (FN+1 past 2) and `fader.filter` (FN+4) — declared at `v26map.ts:39-41`, emitted at `surface.ts:844-852`, with the fader claim ladder at `surface.ts:815-830` (`"headScrub" | "headLevel" | "window" | "fader"`).
- Heads already claims the fader layer ahead of the FN window rows (`surface.ts:818`), and Heads reverse is already a Track double-tap, not a fader double-tap (`surface.ts:588`).
- Track buttons today: double-tap = `track.delete` (`surface.ts:485-492`), tap = mute/unmute (`:529-532`), hold = `rec.arm`/`rec.requestInput` (`:645+`), Heads hold = source/PRINT (`:612-622`), FN+Track = bank/song (`:466-477`).
- Taps fire **optimistically** at `count = 1` then re-fire at `count = 2` (`src/input/gestures.ts:104-113`, `DEFAULT_TIMINGS` `multiTapGapMs: 300`, `holdMs: 450`). This is the root blocker for every deferred gesture below.
- `PLAY + Track` latching solo + long-chord link/unlink: `stemTapeV1Map.ts:92-108`, `surface.ts:890-894`, arbitrated in `src/machine/chordArbiter.ts`.
- FX overlay Track rows: `stemTapeV1Map.ts:213-260`, already declaring `suppresses: ["track.mute","track.unmute"]`.
- Grid is scalar tap-tempo only (`src/audio/grid.ts`, persisted as `control.grid = { bpm, source }` in `src/audio/store.ts`).
- Dual-source equal-power relocation with per-track generation counters already exists (`engine.ts:820`, `:926`, `:1139`, `:143`, `:679`); audibility funnels through `applyAudibility` (`engine.ts:1230-1245`).
- `src/audio/export/performanceRecorder.ts:30,43,47` loads `public/input-capture-processor.js` **and** `src/workers/recordingWorker.ts`; `src/workers/wavWorker.ts:9` imports `chunkStore`. These stay regardless of recording/PRINT removal.

## 1. Final mapping tables

### Universal (identical in Tape, Heads, FX overlay)

| Gesture | Command | Target |
|---|---|---|
| FN + Fader 1–4 | `lane.scrub.start/update/end` | that physical lane's source |
| FN + double-tap Track 1–4 | `lane.reverse` | that physical lane |
| FN + Track + Volume − | `loop.resize { dir:-1 }` | that lane's active loop |
| FN + Track + Volume + | `loop.resize { dir:+1 }` | that lane's active loop |

FN + Rocker stays the **global** four-stem shuttle — untouched, and per-lane scrub must never emit it.

### Tape mode

| Gesture | Command |
|---|---|
| Track tap | `track.mute` / `track.unmute`; if looping → `loop.release` |
| Track hold | `audition.start/update/end` (momentary solo) |
| Multiple Track holds | audible group = exactly the held set |
| Track double-tap | `loop.capture` |
| PLAY + Track short | `stem.solo` (persistent latch) — unchanged |
| PLAY + Track long | `stem.link` / `stem.unlink` — unchanged |
| FN + Track single | bank / song navigation — unchanged |

### Heads mode — four independent head lanes

Heads state is keyed by `laneId` (1–4), and `sourceStem` lives **inside** the lane record — it is not "per-stem". Default on entry is unchanged: all four lanes take the currently selected stem at offsets 0 / 25 / 50 / 75 %, preserving the existing symphony effect. The Heads panel may then reassign any single lane to a different stem.

Each head lane scrubs, loops and reverses independently through the same universal gestures as Tape.

Changing a lane's source is a transaction, in order: release that lane's active loop safely (at the next boundary, hidden-pointer rejoin), clear that lane's scrub candidate, equal-power crossfade the lane from old to new source at the lane's current read position, bump that lane's generation counter. The other three lanes are never restarted, repositioned or re-levelled.

FN + Track single has **no** action in Heads (source moves to the panel). Track hold no longer selects source and never triggers PRINT. Track tap on a looping lane releases that lane's loop instead of muting.

### FX overlay

Bare Track = FX-bank select; Track hold = momentary FX; Track + Vol −/+ = algorithm cycle; FN + Track **single** = latch/unlatch — all unchanged. **A bare Track tap in the FX overlay never releases a loop**: active loops keep playing and are routed through the selected FX. Loop release is available again as soon as the overlay is closed. The four universal FN-qualified gestures (scrub, reverse, resize) work inside the overlay and target the **physical stem/head lane**, not the FX bank.


## 2. Displaced mappings and their new homes

| Removed | New home |
|---|---|
| `fader.window` (FN+1/2/3 window start/end/shift) | **Advanced Loop panel** (new UI in the Tape page) driving the existing `window` engine commands unchanged |
| `fader.windowReverse` (start past end) | Advanced Loop panel derives it from start/end exactly as `surface.ts:848` does today |
| `fader.filter` (FN+4) | Retired — Filter remains in the TONE FX bank |
| `heads.source` / `heads.print` Track hold | **Heads panel**: per-lane source assignment among Vocals / Drums / Bass / Instruments, persisted per song as `StoredProject.control.heads.lanes[laneId] = { sourceStem, offsetPercent, reverse }` |
| `heads.reverse` (Heads-only) | Replaced by universal `lane.reverse` |
| `track.delete` (Track double-tap) | Removed from the surface entirely |
| all `rec.*` Track gestures | Removed |

Both the old rows (`v26map.ts:39-41`, the emissions at `surface.ts:844-852`) and the new commands must not be simultaneously reachable — the fader claim ladder at `surface.ts:815` becomes `"laneScrub" | "headLevel" | "fader"`, with `"window"` deleted.

## 3. Ordered gesture arbitration

New `src/input/laneGestures.ts` sits between `gestures.ts` and `surface.ts`. `gestures.ts` stops firing optimistic `count = 1` for Track buttons; it emits raw `down/up/cancel` and the arbiter decides.

**Track arbiter (per control):**

```text
idle
 ├─ down ────────────────► pressed
 pressed
 ├─ t ≥ holdMs ──────────► auditioning        (cancels pending tap window)
 ├─ up ──────────────────► awaitingSecond (≤ trackTapWindowMs, 200ms, tunable 180–220)
 awaitingSecond
 ├─ timeout ─────────────► tapConfirmed
 ├─ down ────────────────► doubleClaimed      (claim only — nothing emitted yet)
 doubleClaimed
 ├─ up before holdMs, no chord qualifier ──► doubleConfirmed → loop.capture
 ├─ t ≥ holdMs ────────────────────────────► auditioning (double abandoned)
 ├─ FN / PLAY / Volume qualifies the press ─► that chord wins, double abandoned
 ├─ cancel|blur|lostpointercapture ────────► release("cancel"), nothing emitted
 auditioning / any ── cancel|blur|lostpointercapture ──► release("cancel")
```

The second press **claims** the double-tap on pointer/key down (so no tap-level mute can fire), but `loop.capture` is **finalized only on that second press's valid release**. A second press that turns into a hold, is cancelled, or becomes part of a qualified chord never captures a loop.

**FN-qualified arbiter (runs first when FUNCTION is held):**

```text
fnTrackDown
 ├─ Volume −/+ pressed while Track held ─► resizeClaimed   (highest precedence)
 ├─ second Track down within window ─────► reverseClaimed (finalized on valid release)
 └─ window expires, Track released ──────► fnSingleClaimed (layer-specific)
```

Precedence, evaluated in order: FN + Track + Volume → `resize`; FN + double-tap Track → `reverse`; FN + Track single → layer action; bare Track double-tap → `loop.capture`; bare Track hold → `audition`; bare Track tap → mute/release (bank-select in the FX overlay, never release).

**Suppression sets** (declared on the map rows so the audit table is generated, not hand-written):

- `resize` suppresses: `volume.master*`, `track.mute/unmute`, `audition.*`, `loop.release`, `lane.reverse`, `song.bank*`, `fx.latch`, `fx.cycle`, `function.tap`, all `transport.*`.
- `reverse` suppresses: two `fnSingle` emissions, `song.bank*`, `fx.latch`, `track.mute/unmute`, `loop.capture`, `audition.*`, `function.tap`.

No optimistic command fires before the arbiter resolves. PLAY held at Track-down means the chord arbiter wins and no audition begins.

## 4. Momentary audition + persistent solo (both kept)

`src/machine/audition.ts` (new, pure): `inactive → active(heldSet)`. First hold snapshots `{mutes[], solos[]}`; held set defines the audible group; releasing a member updates it; releasing the last member restores the snapshot byte-for-byte and clears. Never persisted, never written to `StoredProject`. Persistent solo/link stays exactly as it is in `chordArbiter.ts`.

## 5. Universal audible scrub

`lane.scrub.start` claims the fader's continuous channel **before** the first movement, so `fader.trackVolume` never fires during the gesture and the stored level is untouched (release restores the stored value — no jump).

Per lane the engine runs a scrub grain path (reusing the existing global-shuttle grain machinery, but per-track and per-generation) while the lane's hidden song pointer keeps advancing on the integrated rate curve. Release schedules one handoff frame, invalidates the scrub generation, complementary-fades the grain out and the normal source in — the same ≤2-frame landing contract already proven for the global shuttle. Other lanes are untouched; multiple simultaneous FN+Fader scrubs are independent, one generation counter each.

```ts
interface ScrubCandidate {
  sourceStem: TrackId;
  sourceFrame: number;
  capturedAtContextFrame: number;
  sourceGeneration: number;
}
```

Ephemeral only. Replaced by the next completed scrub; cleared on song switch, source replacement or generation mismatch; consumed by `loop.capture`. Surfaced in diagnostics and as a distinct LED/panel indicator.

## 6. Shared PerformanceLoop model

One engine, `src/audio/performanceLoop.ts`, keyed by physical lane — used identically by Tape and Heads. No nested systems. The existing window/chop engine is untouched and is **not** routed through the link mask.

```text
inactive → captureScheduled → looping ⇄ resizeScheduled → looping → releaseScheduled → inactive
```

State per lane: `sourceStem`, `startFrame`, `endFrame`, `lengthBars`, `origin: "grid" | "scrub"`, `audiblePointer`, `hiddenSongPointer`, `reverse`, `applyAtFrame`, `sourceGeneration`.

- **Capture:** scrub candidate present → `startFrame = candidate.sourceFrame`; otherwise the grid bar containing the accepted double-tap frame. Default length 1 bar. Activation scheduled at the next shared bar boundary. Candidate consumed. Only that lane loops.
- **Resize:** ¼ ↔ ½ ↔ 1 ↔ 2 ↔ 4 ↔ 8 bars, clamped; start frame fixed; duration from the beat/bar frame map; applied at the next loop boundary with a crossfade; fader never automated. No active loop → reject `loop.resize` only, with an explicit ack.
- **Release:** crossfade from loop grain to a fresh source at the hidden pointer at the next boundary, then tear down so exactly one playback path remains. Rejoin ≤2 frames. Beyond source end → silence, no wrap or stretch.
- Worklet (`public/tape-processor.js`) receives the same window + hidden pointer fields for node/worklet parity.

## 7. Universal reverse

`lane.reverse` (new semantic command) replaces `heads.reverse`; the old name is deleted rather than overloaded, because its state names would misdescribe Tape and FX layers.

- **Loop active:** reverse the read direction inside the captured bounds; start, length and hidden pointer preserved; applied at the next safe boundary with a crossfade.
- **No loop:** create a reversible performance playback layer for that lane starting at its current audible source position; the hidden forward song pointer keeps advancing; toggling off crossfades back to the hidden position. Other lanes never restart or reposition.
- Worklet uses the negative read step (no extra PCM). Node fallback uses the existing gated reverse-copy path and its allocation goes through the operation-level memory gate in `src/audio/memory.ts`.

## 8. Automatic grid + portable persistence

`src/audio/gridAnalysis.ts` + `src/workers/gridWorker.ts`, run after ingest/restore, before performance-ready. Reads already-decoded channel data in sequential bounded chunks (no second decode, no cloud, no AI terminology, no confidence score). Spectral-flux onset envelope → autocorrelation/comb tempo over 60–200 BPM with explicit half/double disambiguation → phase by pulse-train correlation → downbeats by 4-beat energy accumulation.

The grid belongs to the **song timeline**, not to any individual stem's length. Time in seconds is the authority; frames are derived.

```ts
interface SongGrid {
  gridId: string;
  beatsPerBar: number;
  analysisSampleRate: number;      // rate the analysis ran at
  analysisOriginFrame: number;     // origin in analysis frames
  songDurationSeconds: number;     // song-timeline duration at analysis time
  beatTimesSeconds: Float64Array;  // AUTHORITATIVE
  barTimesSeconds: Float64Array;   // AUTHORITATIVE
  beatFramesAtAnalysis: Int32Array; // original analysis frames (audit)
  normalized: Float64Array;        // cross-check only
  segments: { startSeconds: number; bpm: number; source: "auto" | "manual" }[];
  sourceHashes: string[];
}
```

**Restore:** primary computation is `frame = Math.round(beatTimeSeconds * decodedContextSampleRate)`. The rate-scaled form (`beatFramesAtAnalysis * ctxRate / analysisSampleRate`) and `normalized * songDurationFrames` are computed as **validators only**. Disagreement beyond 2 frames logs a diagnostic and keeps the seconds-derived value; disagreement beyond 50 ms marks the grid stale and re-runs analysis. `normalized` is never used to reconstruct positions when seconds are present, so unequal stem lengths, encoder padding and small decode-duration drift cannot shift the grid.

Persisted additively on `StoredProject.control.grid`; recomputed when `sourceHashes` change. FN ×4 tap remains the optional manual correction, writing `source: "manual"` segments.

**Grid consumers:** Tempo Echo, Pitch Echo (when synchronized), Reel Flange modulation (when synchronized), Rhythmic Gate, performance loops, chop and other grid transitions. Formant Shift does not consume the grid.

## 8b. FX registry correction — Pump and Beat Repeat removed

Button 4's bank is final: **MOD → Reel Flange → Formant Shift → Rhythmic Gate**.

Every reference is updated in one pass: FX registry and bank names (`src/audio/fx/banks.ts`), `fx.*` commands and acknowledgements, persistence schema + migration, diagnostics, LED/readout labels, mapping export, DSP tests, grid consumers.

**Deleted:** Pump DSP and its tests; Beat Repeat processor (`public/beat-repeat-processor.js`) and its dedicated ring-buffer infrastructure. Shared LFO, delay-line and worklet utilities are **retained** wherever another retained effect still uses them; deletion is only permitted after a reference check shows zero remaining importers.

**Saved-project migration:** Beat Repeat slot → Reel Flange; Pump slot → Formant Shift; Rhythmic Gate state preserved verbatim. Migration clears any momentary or latched activation on the two remapped slots, so a project can never load with a different effect unexpectedly engaged. Parameter values from the removed effects are dropped, not reinterpreted; the replacement loads at its documented defaults.


## 9. Removal dependency audit

**Recording — remove (reachable surface):** mic permission path, input selection, arming, overdub, punch-in, monitoring, latency-compensation UI, take-management UI, `src/audio/InputPanel.tsx` and its mount at `SystemPage.tsx:340`, all `rec.*` emissions in `surface.ts`, `rec.*` rows in `stemTapeV1Map.ts:284-340`, recording LED tiers.

**PRINT — remove:** `heads.print` at `v26map.ts:70`, `stemTapeV1Map.ts:381`, `surface.ts:621-622`, `commands.ts:65`, `engine.ts:939` and `:3199`; PRINT render path, progress/failure LED tiers, PRINT diagnostics and state; `src/audio/print.ts` plus its sole importer line `useAudioEngine.ts:5`; PRINT tests.

**Retain (shared with retained features):** `public/input-capture-processor.js` and `src/workers/recordingWorker.ts` (`performanceRecorder.ts:30,43,47`), `src/audio/input/chunkStore.ts` (`wavWorker.ts:9`, `takePageWorker.ts`), `src/audio/wav.ts`, `src/audio/export/*`, `src/audio/ingest.ts`, original uploaded blobs, previously printed stems (now ordinary project audio).

**Legacy takes:** preserved non-destructively and invisibly for one compatibility cycle — `takes.ts`, `takePages.ts`, `takePageWorker.ts` and the take manifest fields stay so existing projects keep playing. No export interface, no new UI, no new takes created. Deliberate cleanup proposed after migration is proven.

**Removable after the above:** `src/machine/recordingState.ts`, `src/audio/input/recorder.ts`, `src/audio/input/latency.ts`, recording/PRINT sections of `src/audio/__tests__/phase6.test.ts`.

## 10. Commands and schema

```
track.mute / track.unmute { track }
audition.start { held[], snapshotId } | audition.update { held[] } | audition.end { snapshotId }
lane.scrub.start / lane.scrub.update / lane.scrub.end { lane, sourceFrame }
lane.reverse { lane, on }
loop.capture { lane, startFrame, endFrame, lengthBars, origin, activateAtFrame }
loop.resize  { lane, dir, lengthBars, applyAtFrame }
loop.release { lane, releaseAtFrame }
grid.installed | grid.replaced | grid.correct
```

Schema additions: `control.grid` → `SongGrid` (seconds-authoritative); `control.heads.lanes[laneId] = { sourceStem, offsetPercent, reverse }`; `heads.lane.setSource { lane, sourceStem }` command (releases loop, clears candidate, crossfades that lane only). FX slot migration as in §8b. Removed: PRINT state fields where no legacy record references them. Rejections ack with a reason and mutate nothing.

## 11. Diagnostics and LEDs

`window.__stemTape` gains, per lane: source stem, layer, scrub active, scrub read frame, stored candidate, loop state, start/end, length in bars and frames, capture origin, reverse, audible pointer, hidden pointer, scheduled transition frame, rejoin error, playback-path count, last ordered gesture, suppressed commands, last rejection.

LED arbitration gains distinct tiers for momentary audition, persistent solo, loop scheduled, loop active, resize accepted, reverse, loop release, alongside existing FX / Heads / error tiers. When loop and solo/Heads coexist, a deterministic alternating composite pattern keeps the loop visible. All LEDs stay derived centrally in `surface.ts`; handlers never write them.

## 12. Implementation order

1. Remove reachable recording + PRINT mappings/UI; keep shared infra and legacy data.
2. Unified Track and FN-qualified gesture arbitration (`laneGestures.ts`, deferred taps).
3. Momentary audition; retain PLAY+Track persistent solo/link.
4. Replace FN+Fader window/filter with universal per-lane scrub; add the Advanced Loop panel.
5. Heads source-selection panel; remove Heads source/PRINT hold.
6. Automatic grid analysis + persistence.
7. Shared PerformanceLoop: scrub capture, resize, hidden timeline, release.
8. Universal reverse in node and worklet.
9. FX registry correction (Pump/Beat Repeat → Formant Shift/Reel Flange) + saved-project migration; rewire grid consumers to the frame map.
10. LED/diagnostic changes + full browser/device matrix.

## 13. Evidence that will be produced

**Unit (vitest, frame assertions with stated tolerances):** gesture matrix — bare single/double/hold exclusivity, FN+Track single/double/triple exclusivity, all 2/3/4 hold combinations, random release order, blur and pointer cancel, keyboard and touch; audition snapshot restore equality; grid fixtures (straight 4/4, sparse intro, half/double ambiguity, drift, 44.1/48 kHz, unequal lengths, reload, stem replacement); loop capture with and without scrub candidate, candidate consumption, exact 1-bar default, frame-correct ¼→8-bar steps, fixed start during resize, 1–4 simultaneous loops at different lengths, hidden pointers advancing, rejoin ≤2 frames, exactly one playback path after release, node/worklet parity, no leak over 100 cycles; reverse in all three layers, other lanes forward, toggle-off rejoin ≤2 frames, worklet adds no PCM, node extra PCM memory-gated, zero duplicate sources; scrub in all three layers — correct lane claimed, audible grain output, read-frame movement, hidden pointer continues, exact landing frame stored, fader volume unchanged, clean crossfade, **no global-shuttle command emitted**.

**Playwright (Chromium + WebKit):** zero `getUserMedia`; no recording or PRINT controls in the DOM; no `rec.*` or `heads.print` reachable in the command stream; no PRINT LED/state; previously printed stems load as ordinary project audio; master performance recording and WAV export succeed; zero user audio in network requests; zero console errors; TypeScript clean.

**Real device (iPhone/iPad):** 2/3/4-finger Track holds, loop capture/resize/release listening test, FN+Fader scrub per lane, background-interruption recovery.

**Measured latencies reported:** bare Track tap, FN+Track single, FN+double-tap Track, FN+Track+Volume resize — with the final chosen `trackTapWindowMs` and `holdMs`.

Added to §13: per-lane head source reassignment leaves the other three lanes' pointers and levels bit-identical; FX-overlay Track tap selects a bank while an active loop keeps sounding through it and is still releasable after the overlay closes; second-press-that-becomes-a-hold never emits `loop.capture`; grid restore at 44.1 ↔ 48 kHz agrees within 2 frames across all three representations; migrated Beat Repeat/Pump projects load as Reel Flange/Formant Shift with zero active momentary or latch state and Rhythmic Gate untouched.

## 14. Unresolved decisions

None. Both previously open questions are resolved by this revision: Heads is four independent lanes keyed by `laneId` with per-lane `sourceStem` (§1), and an FX-overlay bare Track tap selects the bank and never releases a loop (§1).
