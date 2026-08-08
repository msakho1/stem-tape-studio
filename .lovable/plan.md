# Phase 4.1 Memory Correction (approved, with corrections) + Phase 5 Plan

## Part A — Phase 4.1 implementation

### A0. Reproduced decision path (verified in code)

`src/audio/engine.ts:184-198`
```ts
const buffer = await this.ctx.decodeAudioData(bytes.slice(0));
const projected = this.decodedTotalBytes - existing + incoming;
if (judge(projected, this.budget) === "block") { /* "memory budget exceeded — 199 MB …" */ }
```
The "vocals — " prefix is added by the caller, `src/audio/ProjectDrawer.tsx:52`.

1. **199 is the projected retained decoded project total** (all engine tracks + this one), not the vocal file, not the transient peak. The per-role prefix is the mislabel.
2. Thresholds live at `src/audio/memory.ts:22` — iOS warn 96, block 192, computed with `1024*1024` but labelled "MB".
3. **Every track currently decodes twice**: `probeFile` full-decodes (`format.ts:138-139`) and throws the buffer away; `ingest.ts:49-50` re-reads the file and `engine.loadTrack` decodes again.
4. SSR budget (180) vs iOS budget (96) produces the live hydration mismatch on `memory-meter`.

### A1. Units and thresholds
**MiB (1024²) everywhere** — thresholds, calculations, diagnostics, error strings, UI. `formatBytes` relabelled `KiB/MiB`.

```ts
ios: { warn: 192, standardBlock: 384, highMemoryBlock: 512 }  // MiB
```
Bands: `<192` normal · `192–384` warning, allowed · `384–512` requires High Memory Mode · `>512` refuse full decode, offer Memory Saver. Android/desktop get the same 4-tier shape with their own numbers, all overridable from `/bench`. `navigator.storage.estimate()` stays labelled *device storage* and never feeds the RAM verdict.

Message wording (corrected):
- `Project total: 199 MiB. This exceeds the 192 MiB warning threshold but remains below the 384 MiB standard limit. Loading is allowed.`
- `Project total: 420 MiB. This exceeds the 384 MiB standard limit. Enable High Memory Mode to attempt loading up to 512 MiB.`
- `>512`: refusal naming the 512 MiB high-memory ceiling and the Memory Saver alternative.

### A2. Two-stage gating + single decode
1. **Pre-decode estimate** from sniffed header (WAV: fmt chunk gives exact frames; MP3/M4A/FLAC: duration often unknown → estimate flagged `uncertain`, conservative policy = assume stereo at context rate and use the pessimistic bound) plus current retained total. Reject / demand High Memory Mode *before* allocating.
2. **One full decode** (the probe decode), which now **returns its AudioBuffer**.
3. **Exact post-decode verdict** from `buffer.length * channels * 4`.
4. Accept → `engine.adoptBuffer(track, buffer, meta)`. Reject → dereference the buffer immediately.

`engine.loadTrack(bytes)` remains only as an explicitly documented fallback; taking it after a successful probe increments `decodeCount` to 2 so the test can prove it never happens. Restore path: `getBlob` → one decode → adopt.

**No encoded copy:** `bytes.slice(0)` is dropped. `file.arrayBuffer()` already returns an owned buffer; `decodeAudioData` detaches it, and the persistence source is the `File`/`Blob`, so duplication is unnecessary. Any browser that is observed to require a copy will be documented at the call site with the observed failure.

### A3. Sequential decode and release
Queue: decode one → adopt → null the encoded reference → macrotask yield → next. Cancellation token aborts remaining work; failed, cancelled and replaced stems release intermediates and revoke object URLs. Diagnostics say "dereferenced (GC timing not controlled)", never "freed".

### A4. Reporting (project drawer, MiB)
Per stem: file size, duration/channels/rate, estimated decoded MiB, estimate certainty. Totals block: retained total · estimated peak during load · reverse-copy cost (P5) · warn/standard/high thresholds · current mode · status sentence. With <4 roles present, show current selected total **and** projected four-role total, labelled an estimate. No single stem is ever compared to a project threshold.

### A5. High Memory Mode
Opt-in toggle, offered only in the 384–512 MiB band, exact warning copy from the brief, disableable, recorded in diagnostics, not sticky across projects unless the user opts in. Original blobs survive any failed load.

### A6. Memory Saver (persisted derived copy)
Explicit alternative; predicted saving shown before applying; one track at a time; never mutates the original.
Derived record stored alongside the original blob:
```
{ derivedKey, sourceBlobKey, sourceContentHash, conversion: { type:"mono-downmix", sampleRate }, derivedSchemaVersion }
```
Reopen uses the derived blob directly — the full stereo source is not decoded again. Derived copies can be deleted and regenerated. `StoredStem` gains these fields behind a store schema bump.

### A7. BPM model
```ts
type BpmSource = "manual" | "tempo-grid" | "provisional";
baseBpm = tempoGridBpm ?? manuallyEnteredBpm ?? 120;
```
Optional BPM field in the drawer; FN ×4 tempo-grid taps establish/update BPM; until then 120 shows as **"120 BPM — provisional"** (never "detected"). A newly available real BPM affects future ±1 BPM steps only and does not change the current audible rate. Exact semitone stays BPM-independent. BPM value + source persist per song (`StoredProject.control.grid` already carries `{bpm, source}`).

### A8. Verification (real browser, 375 px, iOS UA)
Assert: 199 MiB project loads with the warning sentence; `decodeCount === 1` per track; no SSR hydration mismatch; MiB labels consistent; pre-decode gate rejects before allocation and post-decode gate applies the exact verdict; standard vs High Memory bands behave; a persisted Memory Saver copy reopens without decoding the original; **zero network requests containing user-selected or user-recorded audio (bundled demo assets may legitimately be fetched)**; save/reload; cancel cleanup; faders responsive during and after load; TypeScript clean. Exact diagnostics returned verbatim.

## Part B — Phase 5 (planned after 4.1 passes, no DSP yet)
Deliverable will contain: AudioEngine audit (what survives: command stream, acks, txn rollback, control bus, derived anchor, ProjectStore; what extends: loop/window/filter/per-track rate, EngineStatus, memory approval per operation); the full command-to-audio matrix for `rocker.speed`, `rocker.semitone`, `play.snap`, `fader.filter`, `fader.window`, `fader.windowReverse`, `play.loopMode`, `rocker.chopHalf/Double/Reset`, `volume.chopWindow`, per-track loop and rollback rows, each with gesture, reducer mutation, engine action, continuous/discrete, scheduling boundary, anti-click method, rollback, memory cost, P5A/P5B placement and test; **P5A** node-based tape core; **P5B** worklet engine; resolved AudioWorklet feasibility (PCM transfer, SharedArrayBuffer/COOP-COEP and whether hosting supplies the headers, iOS Safari, fallback, startup/teardown/recovery, MessagePort bandwidth, per-track vs shared processor, memory ownership, OPFS chunked reads, worker preprocessing, migration keeping GestureEngine/SVG untouched); loop/chop terminology and sample-frame state model; memory-safe reverse; click-prevention with rendered-output discontinuity measurement; diagnostics additions; test and browser matrix; ordered implementation steps and acceptance criteria.
