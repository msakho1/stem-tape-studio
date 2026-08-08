# Phase 4.1 Memory Correction + Phase 5 Tape Manipulation Plan

## Part A — Phase 4.1 (implement on approval)

### A0. Reproduced decision path (verified in code, not inferred)

The iPhone message comes from one place:

`src/audio/engine.ts:184-198`
```ts
const buffer = await this.ctx.decodeAudioData(bytes.slice(0));
const incoming = decodedBytes(...);
const existing = track.buffer ? decodedBytes(...) : 0;
const projected = this.decodedTotalBytes - existing + incoming;
if (judge(projected, this.budget) === "block") {
  return { ok: false, detail: `memory budget exceeded — ${(projected / 1048576).toFixed(0)} MB decoded exceeds the ${(this.budget.blockBytes / 1048576).toFixed(0)} MB ${this.budget.platform} block threshold` };
}
```
and the "vocals —" prefix is added by the caller, `src/audio/ProjectDrawer.tsx:52`: `note(r.ok, `${ROLE_LABEL[role]} — ${r.detail}`)`.

Findings:
1. **199 MB is category (e) — the projected *retained decoded total* of all tracks already in the engine plus this one.** It is not the vocal file, not the transient peak. Presenting it under "vocals" is the mislabel to fix.
2. **Threshold source:** `src/audio/memory.ts:22` — iOS `warnBytes = 96 MiB`, `blockBytes = 192 MiB`.
3. **Unit bug:** every divisor is `1048576` / `1024*1024` (`format.ts:118-122`, engine message) but the label says "MB". Units are MiB today, labelled MB.
4. **Duplicate decode confirmed.** `probeFile` performs a full `decodeAudioData` (`format.ts:138-139`), discards the AudioBuffer, and `ingestStem` (`ingest.ts:49-50`) re-reads the file and `loadTrack` decodes the *same bytes a second time* (`engine.ts:184`). Every track decodes twice today, and peak memory holds two full buffers of the same stem.
5. **SSR/client threshold mismatch** (present runtime error): `defaultBudget()` returns 180 MiB on the server and 96 MiB on iPhone, so the memory meter hydration-mismatches. Fixing budget resolution to a post-hydration effect removes it.

### A1. Threshold policy (configurable)

Single unit: **MiB (1024²), labelled `MiB` everywhere** — in calculations, error strings and UI. No more "MB" on a binary number.

```ts
iosWarn = 192 MiB; iosStandardBlock = 384 MiB; iosHighMemoryBlock = 512 MiB;
```
- `< 192` load normally · `192–384` load with warning · `384–512` requires explicit High Memory Mode · `> 512` refuse full decode, offer Memory Saver / future streaming.
- Same 4-tier shape for android and desktop with their own numbers, all exported from `memory.ts` and overridable from `/bench`.
- `navigator.storage.estimate()` stays where it is — labelled **device storage**, never mixed into the RAM verdict.

### A2. Single-decode ingest

`probeFile` gains an option to **return the decoded AudioBuffer** it already produced. `ingestStem` then:
sniff → one decode (probe) → budget verdict on the projected total → `engine.adoptBuffer(track, buffer, meta)` → persist original Blob → drop the encoded ArrayBuffer reference.

`engine.loadTrack(bytes)` is kept only as the documented fallback path (used if adoption fails), and any use of it after a successful probe is recorded as `decodeCount: 2` in diagnostics so the acceptance test can prove it never happens.

Restore path: `getBlob` → one decode → adopt. Same counters.

### A3. Sequential decode + release

Demo load and multi-file selection run through a queue: decode one, adopt, null the encoded buffer, `await` a macrotask yield, next. Cancellation token aborts the queue; failed/cancelled/replaced stems release their intermediates and revoke any object URLs. Diagnostics wording is explicit: "dereferenced (GC timing not controlled)", never "freed".

### A4. Memory reporting UI (project drawer)

Per stem: file size, duration/ch/rate, estimated decoded MiB. Then:
`Retained total · Estimated peak during load · Reverse-copy cost (P5) · warn / block / high-memory thresholds · mode · status`.
When fewer than four roles are present, show **current selected total** and **projected four-role total** (extrapolated from loaded stems, labelled as an estimate). Never compare one stem against a project threshold.

Rejection text becomes: `project total 199 MiB (vocals contributes 49 MiB) exceeds the 384 MiB iOS block threshold`.

### A5. High Memory Mode & Memory Saver

High Memory Mode: opt-in toggle in the drawer, only offered in the 384–512 MiB band, with the stated warning text, disableable, recorded in diagnostics, not sticky across projects unless the user ticks "remember".
Memory Saver: explicit alternative offering mono downmix (per selected stem), shorter edit, or fewer tracks — each showing predicted saving *before* applying, processing one track at a time, writing a separately labelled working copy, never mutating the stored original.

### A6. Acceptance test (Playwright, 375 px viewport, iOS UA)

Synthesises a ~199 MiB four-stem fixture and asserts: allowed-with-warning; four stems decoded with `decodeCount === 1` each; start spread 0.000 ms; fader response during and after load; zero network requests carrying audio; save/reload; cancel cleanup; per-track/total/peak numbers match computed expectations; 384–512 band blocked until High Memory Mode is on; >512 refused. Verbatim diagnostics returned in the reply.

## Part B — Phase 5 plan (no DSP written until approved)

### B1. Engine audit outcome
Survives unchanged: ordered command stream + acks (`commands.ts`), txn rollback (`surface.ts` `TxnSnapshot`), control bus, derived-anchor playhead, ProjectStore, provenance.
Needs extension: `AudioEngine` (loop/window/filter/per-track rate), `EngineStatus` (loop + memory fields), reducer already carries `speed/chopDiv/window/filter/loopMode` (`surface.ts:60-67`) so no map changes; `memory.ts` gains operation-level approval.

### B2. Command matrix (each row: gesture · reducer mutation · engine action · continuous/discrete · scheduling boundary · anti-click · rollback · memory cost · phase · test)
`rocker.speed`, `rocker.semitone`, `play.snap`, `fader.filter`, `fader.window`, `fader.windowReverse`, `play.loopMode`, `rocker.chopHalf/Double/Reset`, `volume.chopWindow`, per-track loop rows, rollback rows. Documented v2.6 mapping is not altered.

### B3. Phase 5A — node-based tape core
Shared varispeed (all linked sources one `playbackRate`), exact `2^(n/12)` semitone, snap to exactly 1.0, Biquad LP/off/HP per track, shared loop window via `loopStart/loopEnd` on source nodes, chop half/double/reset, fixed vs variable loop, per-song persistence, continuous audible fader/window control, ramped boundaries.

### B4. Phase 5B — AudioWorklet engine
Click-free arbitrary boundaries, bidirectional reverse by negative read pointer (no reversed buffer copies), independent per-track loops, chop glide, multiple heads, scrubbing, relinking, memory-efficient long songs.

### B5. Worklet feasibility questions answered in the deliverable
PCM transfer strategy (transfer vs copy), SharedArrayBuffer + COOP/COEP need and whether hosting can supply the headers, iOS Safari behaviour, non-SAB fallback, startup/teardown/recovery, MessagePort bandwidth, per-track vs shared processor, memory ownership, OPFS chunked reads, worker preprocessing, and a migration path that keeps GestureEngine and SVG untouched.

### B6. Loop/chop state model
Distinct definitions and storage for song range, chop window, loop window, fixed vs variable, per-track vs shared, reversal — all stored in **sample frames relative to the source**, with defined behaviour across rate change, song switch, unequal stem lengths, start-crosses-end, relinking, LED mapping.

### B7. Click prevention & memory safety
Measured discontinuity tests via OfflineAudioContext around every boundary (play/stop/restart/mute/wrap/resize/move/reverse/filter/source-recreate/relink). Reverse and every worklet allocation must pass the memory estimator first; a rejected operation rejects only itself, keeps the project loaded, explains why, and offers a cheaper alternative.

### B8. Test matrix
Rate, semitone, ±1 BPM, snap, rollback, no-position-jump, 10-minute drift, loop alignment/independence/100+ wraps/moving boundaries/crossing/fixed-variable/relink, filter sweeps and no-jump-on-FN, memory scenarios (199 MiB, standard, high, rejected reverse, worklet leak, project switching), across iOS Safari, Android Chrome, desktop Safari/Chrome/Firefox/Edge.

## Blocking questions
1. **±1 BPM with unknown project BPM** — `surface.ts:411` currently scales by `(bpmBase + dir)/bpmBase` against an assumed base. Should Phase 5 keep a fixed assumed base (e.g. 120), require the user to enter project BPM, or infer it from the tempo-grid tap gesture?
2. **Unit switch to MiB** — acceptable to relabel all existing UI/diagnostics from "MB" to "MiB", or do you want decimal MB with the divisors changed to 1e6 instead?
3. **Memory Saver mono downmix** — should the working copy be decoded from the original at load time each session, or persisted as a separate stored working blob?
