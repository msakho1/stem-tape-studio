# Phase 4 — Playable Stem Core: sound engine + local project storage

Goal: load four real stems, press the rendered SP-1, hear the mapped actions. No audio code is written yet; this is the plan.

## 1. Integration audit (verified by reading the code)

- `src/input/gestures.ts` (373 lines) — single `GestureEngine`: raw press/release, tap counts with optimistic revision, `holdStart`/`holdEnd` with `level` ("hold" | "power" | "long"), chords, `tapThenHold`, faders excluded from hold/chord. Stays authoritative; no second engine.
- `src/machine/v26map.ts` — 37 rows, each already carrying `status: "state" | "visual" | "audio" | "doc"`. This field becomes the audio-phase gate.
- `src/machine/surface.ts` (659 lines) — `SurfaceState` already holds everything the engine needs: `playing`, `tracks[4].{content,volume,stem,headPos,headReverse}`, `masterVolume`, `speed`, `chopDiv`, `window`, `filter`, `loopMode`, `headsMode`, `song`, `bank`, `grid`, plus `txn` rollback and `songMemory`. Pure reducer, serializable — safe to observe from an adapter.
- `src/device/useDeviceSurface.ts` — reducer host, ready gate (`__stemTapeReady`), fader rAF path writing `cy` straight to the SVG cap and dispatching `faderCommit` only on pointer-up.
- `src/device/DeviceSurface.tsx` / `DiagnosticPanel.tsx` — dumb view + diagnostics.

Key consequence: the reducer already produces the full command surface. The audio layer subscribes; it never touches SVG or the engine.

### Command classification (37 rows)

- **Directly connectable in P4 (node-based):** `play.toggle`, `play.restart`, `fader.trackVolume`, `track.tap` (mute/unmute), `volume.master`, `lights.*`/`songs.memory` (already state/visual), `track.delete` (buffer unload only, with recovery).
- **Next tape phase, still node-based (P5):** `rocker.speed`, `rocker.semitone`, `play.snap`, `fader.filter` (BiquadFilter), `fader.window`/`fader.windowReverse` (loop points), `play.loopMode`, `rocker.chop*`, `volume.chopWindow`.
- **Requires AudioWorklet (P5/P6):** sample-accurate chop glide, click-free bidirectional reverse, independent per-track polyrhythmic loops, `heads.replay`/`heads.scrub`, look-back record buffer, WAV recorder tap.
- **Requires mic/line input (P6):** `track.record`, `heads.print`, `fn.beatmatch` punch-in.
- **Hardware-only / doc:** `songs.transfer`, `songs.length` (browser equivalent = memory budget).
- **Visual only:** `lights.base`, `lights.pulse`, `lights.songRow`, `fn.gridReject`.
- **Ambiguous, needs your ruling:** `track.delete` semantics; `fn.tempoGrid` effect on audio with no known BPM; whether `FN + track` song load stops transport.

## 2. Architecture decision

**Phase 4 uses full decode (Architecture A)** with a hard, enforced memory budget; migration to chunked/AudioWorklet streaming (B) is scheduled for P5/P6 behind the same `AudioEngine` interface.

Reason: `AudioBufferSourceNode` gives one-call sample-aligned starts, correct `playbackRate` varispeed, and trivial loop points — the exact P4 acceptance criteria. Streaming buys nothing until reverse/chop/heads exist.

Budget math (`duration * ctx.sampleRate * channels * 4` bytes):
- 4 × 3:00 stereo @48k ≈ **276 MB** decoded. 4 × 8:00 stereo ≈ **737 MB** — not safe on iOS Safari; will be refused, not attempted.
- P4 caps: warn > 180 MB, block > 320 MB total (tunable), mono downmix offered as a mitigation.
- **Migration trigger:** any of — required duration > 4:00 stereo on mobile, reverse copies needed (doubles cost), or benchmark shows decode > 8 s / a load failure on the device matrix.

A `/bench` route measures decode time, context sample rate, max successful duration, four-track drift, reverse cost, and UI responsiveness, and records results per device.

## 3. Audio graph and lifecycle

One `AudioContext` for the app, created/resumed only inside a user gesture (Play or "enable audio").

```text
per track: AudioBufferSourceNode -> trackGain -> [filter, P5] -> analyser -> masterBus
masterBus -> masterGain -> destination      (later: -> recorder tap -> WAV encoder)
```

- Persistent per track: gain, analyser, filter. Recreated per start: only the source node (one-shot), tracked by generation counter so a stale `onended` cannot mutate state.
- All gain changes use `setTargetAtTime` (~8 ms) — no stepped values, no clicks. Mute = gain to 0, buffer stays loaded.
- Lifecycle handled explicitly: visibility change, backgrounding, lock, `interrupted`/`suspended` states, output route change. Transport LEDs reflect **actual** context state; a suspended context can never render as "playing".

## 4. Synchronization

`startAt = ctx.currentTime + lookahead (≈80 ms)`; all four sources get the identical `startAt` and identical `offset`. Playhead is derived, never incremented:

`position = anchorPos + (ctx.currentTime - anchorTime) * rate * direction`, re-anchored on every rate/transport change.

Acceptance: initial start difference 0 samples (proved by OfflineAudioContext impulse fixture), drift 0 after 10 min, after rate changes, and after pause/resume/restart. Unequal stem lengths align at zero, project length = longest, shorter stems are silence, user warned.

## 5. Ordered command stream (not snapshot diffing)

Audio is never inferred by diffing reducer snapshots — that loses repeated restarts, optimistic multi-tap actions, rollbacks and duplicate identical commands. One ordered stream feeds both consumers:

```text
SVG -> GestureEngine -> v2.6 map -> SemanticCommand{ id, t, type, payload, txnId? }
     -> reducer (authoritative for serializable project state)
     -> AudioEngine (same ordered command, same sequence numbers)
     -> ack { accepted | completed | rejected | failed } -> state, LEDs, diagnostics
```

Rollbacks travel as explicit commands carrying the `txnId` of the optimistic action they revoke, so the engine undoes exactly what it applied. Rejections are first-class: not decoded, memory budget exceeded, context won't resume, missing local blob, source creation failed, unsupported codec. A rejected Play must not light the transport LED.

Required tests: repeated restart; play then immediate stop; optimistic rocker action revoked by double-tap rollback; FN+Play ×2 revised into ×3; two identical master-volume commands; fader preview then commit; rejection with no incorrect LED state.

## 5b. Continuous control bus (live faders)

Fader audio must not wait for pointer-up.

```text
pointer drag -> rAF-coalesced fader preview -> continuous control bus
             -> track GainNode AudioParam (setTargetAtTime)
pointer up   -> faderCommit -> reducer -> persistence
```

Rules: audio changes continuously during the drag; no React rerender while dragging; SVG still never touches an AudioNode; roughly one coalesced update per animation frame; the committed value is byte-identical to the last audible preview value. Pointer cancel follows one documented rule — **reconcile audio back to the last committed value** (ramped, not stepped). The same bus later carries FN + fader window/filter and heads scrub.

## 6. Upload experience and format contract

Separate **project drawer** route — never on the SP-1 artwork. Entry points: load demo song, new song, restore local project, (later) import `.stemtape`.

Uploader: four slots, individual or four-at-once selection, desktop drag-drop, native Files picker on iOS/Android, replace-before-load, per-file metadata preview, manual role assignment with filename inference as a *suggestion* (vocal/vox, drum/percussion, bass/808, instrumental/other/music).

Pre-decode report per file: name, extension, size, duration, channels, source sample rate, role, estimated decoded MB, decodability. Validation: exactly four roles, no duplicates, readable, non-empty, duration compatibility warning, memory budget.

**Validation is header- and decode-based, never MIME/filename alone** — mobile pickers supply missing or wrong MIME types. Each file is sniffed (RIFF/WAVE `fmt ` chunk, ID3/MPEG frame sync, `ftyp`, `fLaC`, `FORM…AIFF`) and then probed with a real short decode. Result is reported as one of: **contract-supported · browser decode-supported · unsupported · malformed · decode failed**.

P4 preferred contract: WAV PCM 16/24-bit, 44.1 or 48 kHz, mono or stereo — described as *preferred*, not "guaranteed", until the browser/device matrix passes. 44.1 vs 48 kHz is never rejected; it resamples into the context rate and the conversion is reported. MP3/M4A-AAC/FLAC/AIFF accepted only when the decode probe succeeds, with an encoder-padding/alignment warning for compressed sources. **No silent downmix** — mono downmix is an explicit, user-chosen mitigation and the original local blob is preserved untouched.

Allowlist (advisory only; sniffing decides): `.wav` (`audio/wav`, `audio/x-wav`, `audio/wave`) preferred; `.mp3`, `.m4a`/`.aac`, `.flac`, `.aif`/`.aiff` probe-gated. Empty or bogus MIME strings are accepted and resolved by sniffing.


## 7. Local storage

Audio never leaves the browser in P4. No Cloud upload.

- **L1 session** — File/Blob in memory, project labelled unsaved.
- **L2 save on this device** — project JSON (schema version, roles, filenames, fader values, mutes, speed, chop, window, filter, grid, song slot, content hashes) in **IndexedDB**; **audio blobs in OPFS where available, IndexedDB fallback**, behind one `ProjectStore` interface so the engine never knows which backend won.
- **L3 portable `.stemtape`** — ZIP container with a custom extension. Originals are preserved with their **own** extensions: `audio/vocals.<original-ext>`, with container/codec/sample-rate/bit-depth recorded in `project.json`. If the user instead chooses "export as WAV", the package is explicitly labelled transcoded and writes real WAV bytes. Compressed bytes are never renamed to `.wav`. Plus `optional/{performance.wav,waveform-peaks/}`. Versioned, recoverable offline.

Safety: `navigator.storage.estimate()` shown as used/quota, `persist()` requested on first save, explicit delete flow, partial-write recovery, and plain-language notes that browser storage can be cleared, incognito is unreliable, and export is the real backup. Nothing is deleted silently.

## 8. Testing

- **OfflineAudioContext unit tests:** identical start sample across four tracks, fader→gain, mute/unmute, master gain, restart offsets, rate math, exact semitone (1.05946), ramp shapes, silence padding for short stems, rapid command bursts.
- **Playwright:** synthetic four-track fixture with impulses at known sample positions, decode, unlock, play/stop/restart, fader drag, mute, suspend/resume recovery, save/reload, missing-blob recovery, storage estimate, memory rejection.
- **Device matrix:** iOS Safari, Android Chrome, desktop Chrome/Safari/Firefox/Edge — decode success, max duration, context rate, start alignment, drift, dropouts, save/reload, background/resume.

## 9. Phase 4 steps

1. `ProjectStore` (IndexedDB + OPFS) and project schema v1.
2. Uploader + validation + decode probe + memory estimator.
3. Bundled short demo project (original CC0-licensed stems, no commercial music).
4. `AudioEngine` (context, graph, transport, scheduled starts, acks).
5. Reducer→audio adapter for play/restart/fader/mute/master.
6. Audio diagnostics block in the existing panel.
7. `/bench` benchmark route.
8. Offline + Playwright suites, then the device matrix.

Acceptance: upload four WAVs on phone or desktop, press Play on the rendered SP-1, hear one synchronized song, mix with the four faders, mute/unmute, stop and restart reliably, reload a saved local project, and confirm no request carries audio.

## 10. Later phases (estimates, high level)

- **P5 Tape Manipulation** — varispeed, semitone, reverse, chop, window, loops, filter. Largest risk: reverse + chop force the AudioWorklet migration.
- **P6 Recording/Grid/Heads** — mic permission, look-back buffer, beat-aligned punch-in, heads, WAV export.
- **P7 Stem layer + hardening** — solo, effects, Mapping Lab outcomes, `.stemtape`, cross-browser, a11y, PWA.
- Physical SP-1 firmware stays a separate track and is excluded from these estimates.

## 11. Resolved decisions (approved)

1. **Track delete** — recoverable project trash. Double-tap unloads/removes the track immediately (gesture fidelity preserved) and shows a brief external "Track deleted — Undo" action outside the SP-1 artwork. The saved blob survives until an explicit save/compact or a trash clear.
2. **FN + track song load** — P4 stops transport, persists the outgoing song's state, loads the target song, and waits for an explicit Play. No crossfade. Flagged for verification against physical Tape Looper hardware.
3. **Mobile ceiling** — full decode with enforced, tunable memory limits; no streaming in P4. Decoded-memory estimate is the primary limit, not a blanket duration cap; platform-specific warn/block defaults are finalized from `/bench` results.
4. **Demo content** — short original/procedurally generated, license-clear four-stem demo with obvious synchronized transients so alignment, mute and fader changes are audible instantly. Replaceable with your own stems later.

