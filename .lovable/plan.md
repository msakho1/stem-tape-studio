# Stem Instrument Mode — MIDI cue learning, iOS CoreMIDI wrapper, Guide card

Plan only. No code in this response. Revised per the corrections of this turn.

## 1. Current-code audit (verified)

MIDI: **none exists.** `rg -ni "midi" src public` returns zero hits (only `webkitAudioContext` at `src/audio/engine.ts:465` and `-webkit-` rules in `src/styles.css:181-182`). No Web MIDI code, no device list, no native wrapper, no Xcode project.

Reusable engine paths (`src/audio/engine.ts`, 4251 lines):

- Voice creation: `private spawn(t, startAt, offset, fadeIn)` `:782`. Builds `AudioBufferSourceNode` + a **per-voice** `fade` GainNode, `fade.connect(t.stemGate)` `:788`, reverse mirroring and loop wrap `:793-801`, equal-power fade-in `sampleCurve(equalPower,"b")` `:812-815`, `node.start(startAt, readOffset)` `:817`, pushes to `t.sources`.
- Voice removal: `private fadeOutAndStop(t, live, at)` `:845` — equal-power `"a"` ramp + `node.stop(at + SEAM_FADE_S)`. **Identity protection**: `if (this.isReleaseTarget(t, live)) return;` `:849`, backed by `pendingRelease[lane]` (the repaired loop-release path).
- Seam constant: `SEAM_FADE_S = 0.012` (`src/audio/crossfade.ts:72`); curves `equalPower`/`complementary` `:24/:30`.
- Lane relocation without touching the shared clock: `respawnLane` `:828`, `relocateLane` `:1021`.
- Shared clock: `timeline.positionAt(t)` / `timeline.anchor(at, pos)` (`:903, :1011, :1137, :3033`); `position()` `:752-758`.
- Global loop: `globalLoop: { start; lengthS; division } | null` `:407`; start handler `:4049`; `releaseGlobalLoop()` `:1074` anchors the shared timeline to the audible frame `:1101/:1137`. **Unchanged.**
- Lane loop: handlers `:3990-3998`; hidden-timeline bar rejoin `scheduleLoopRelease` `:1147-1190` (`hiddenNow` `:1162`). **Unchanged.**
- Audibility: `applyAudibility(id)` `:1650` sets `fxRack.setInputOpen(...)` and `setGain(t.stemGate.gain, …)` `:1665-1667`.
- FX: rack sits ahead of `stemGate` (`stemGate.connect(input)` `:529-531`); buses `normalBus` `:302/:509`, `headsBus` `:1802/:516`; RMS taps `:1891-1896`.
- Momentary audition `:3979`; lane fader scrub `:2846-2968`; Heads voices `spawnHeadVoice` `:1970`, `heads.enter` `:3916`.

Command stream: `src/audio/commands.ts` — union `:14-94`, `AudioCommand` `:99`, ack `:112-120`, `makeCommand` `:128`.

Input/state: `src/machine/surface.ts` — `functionHeld` `:95`, held-Track derivation `:324`/`:746`, `auditionChord` `:169`, `headsMode`/`perf.fxOverlay` `:747`. `src/input/gestures.ts` (496 lines), `src/machine/chordArbiter.ts` (533 lines). Host `src/device/useDeviceSurface.ts` (801 lines), engine handle `:30`.

Persistence: `src/audio/store.ts` — `SCHEMA_VERSION = 2` `:10`, `StoredProject.control` `:52-95`, optional-field precedent `chopDiv?`/`songGrid?` `:69/:76`; `StoredStem.contentHash` `:43`. Single writer `src/audio/session.ts:117 toStoredProject()`.

Guide: `src/device/guideContent.ts` (485 lines) — `Lesson { highlight; motion; held? }` `:250-260`, 20 lessons `:265+`; renderer `src/device/ControlsGuide.tsx:87-101`; illustration `src/device/Sp1GuideIllustration.tsx` over `src/assets/stem-tape-sp1-outline.svg`; coverage test `src/device/__tests__/guideCoverage.test.ts` (asserts exactly 20 lessons `:21-23` and exactly 80 feature ids `:44-47`).

**Correction absorbed:** `stemGate` is per-lane and shared by every voice on that lane, so a cue can *not* be isolated by closing the gate. Cue ownership is therefore expressed **per source voice**: fade the normal voice with `fadeOutAndStop`, spawn the cue voice, and protect the cue voice by identity exactly as `pendingRelease`/`isReleaseTarget` protect a queued release target (`:845-849`).

## 2. Concept

A MIDI key is a **cue marker**, never a pad. No mode toggle, no pad grid, no cue editor, no fixed note bank.

- FUNCTION held + MIDI key = learn a **global** cue (all four stems).
- Exactly one Track held + MIDI key = learn an **isolated** cue for that lane.
- Note On records `startFrame`; Note Off records `endFrame`. Same channel+note relearns overwrite.
- Unqualified Note On plays the **complete learned passage as a one-shot**; Note Off does not shorten it.
- Only marker metadata is persisted. No audio is copied.

## 3. Timestamp → AudioContext conversion (authoritative)

Cue frames are never taken from `engine.position()` at JS-callback time.

- Every `StemMidiEvent` carries `timestampMs` in a monotonic domain aligned to `performance.now()`.
- One calibration pair is maintained: `(perfNowMs0, ctx.currentTime0)`, refreshed on unlock, on visibility change, and on every native re-anchor. `ctxTimeOf(ev) = ctx.currentTime0 + (ev.timestampMs - perfNowMs0)/1000`.
- Cue frames: `frame = round(timeline.positionAt(clamp(ctxTimeOf(ev), ctxStart, ctx.currentTime)) * sampleRate)`. `positionAt` already integrates the rate curve, so a late JS callback still resolves the frame the key was actually pressed at.
- Events whose converted time is older than 250 ms (a stall) are marked `stale: true`, still applied, and reported in the status strip.

## 4. Cue-learning state machine (contiguous and simple)

Keyed by `channel:note`. Concurrent captures on different keys are independent and cannot corrupt one another; a Note Off only closes the capture with the identical `channel:note`.

States per key: `idle → capturing(scope, startFrame) → committed | discarded`.

**Eligibility gate — learning is permitted only during aligned, forward Tape playback.** Learning is rejected, with an explicit reason in the status strip and a `rejected` ack, when any of these hold at Note On:

- Heads mode active
- any scrub in progress (global shuttle or `laneFaderScrub` `:2846`)
- any lane reversed (`t.loop.reverse`)
- the global loop is running (`globalLoop != null` `:407`)
- any lane loop is enabled
- transport is not playing, or the rate is not aligned forward (rate ≤ 0, or |rate − 1| beyond a small tolerance)

Rules:

- Qualifier is read at the Note On instant: FUNCTION held → global. Exactly one Track held → that lane. **Two or more Track buttons held → explicit rejection** ("hold one Track button to learn an isolated cue"); no lane is chosen silently.
- If an eligibility condition becomes true *during* a capture (loop captured, reverse engaged, Heads entered, scrub started), the capture is **discarded** and reported. Learning stays contiguous by construction.
- Commit requires `endFrame - startFrame >= minCueFrames` (proposed 1024 frames); otherwise discarded with a reason.
- Overwrite: a new commit on the same `channel:note` replaces the previous marker in place.
- Cancel: All Notes Off, device disconnect, song/stem change, or transport stop discards all open captures.

## 5. Cue-playback state machine (sample-accurate)

Playback is allowed in far more states than learning: normal play, global loop running, lane loops running, FX overlay open. It is rejected in Heads mode and while stopped it is a no-op (silence).

On unqualified Note On for a learned marker, at `at = max(ctx.currentTime + lookahead, ctxTimeOf(ev))` with `lookahead ≥ SEAM_FADE_S`:

1. Determine the affected lanes: all four for a global cue, one for an isolated cue.
2. For each affected lane: spawn the cue voice with `spawn(t, at, startFrame/sr, true)` `:782`, register it in `cueOwner[lane] = { voice, noteKey, scope, endAt, endFrame }`, and give it the same **identity protection** as `pendingRelease` so no wrap/sweep/relocate can kill it.
3. Fade the lane's previous voice(s) out with `fadeOutAndStop(t, live, at)` `:845`. The two voices overlap for exactly `SEAM_FADE_S`.
4. **At the same moment**, schedule the completion seam: `endAt = at + (endFrame - startFrame)/sr / rate`, using the integrated rate curve for a non-unity rate. The rejoin voice for each lane is scheduled then and there (start time and read offset both computed up front), so the seam is sample-accurate. `tick()` only reaps dead nodes and clears `cueOwner`; it never decides the seam. Any varispeed change during a cue recomputes and reschedules the pending completion, the same way seams are invalidated today (`invalidateSeams`).

Performance semantics:

- **One-shot**: Note Off during playback does nothing.
- **Retrigger**: a new Note On for the same key cancels the pending completion, crossfades to a fresh cue voice at `startFrame`, and reschedules the completion.
- A second, different note takes ownership per §6.
- All Notes Off / device disconnect / transport stop ends the cue at the next seam and rejoins normally (or lands silent if stopped).

Voice invariant (corrected): **one steady-state voice per affected lane; exactly two only during a bounded `SEAM_FADE_S` crossfade; exactly one after it.** Tests assert count ≤ 2 inside the seam window and == 1 outside it.

## 6. Ownership and rejoin

Ownership is per lane, held by `cueOwner[lane]`.

- Isolated cue: owns one lane. The other three are untouched — their voices, loops and hidden pointers keep running.
- Global cue: **temporarily owns all four lanes**. It does not touch `globalLoop`, lane-loop windows, or the shared timeline; those keep advancing underneath.
- A global cue starting while an isolated cue plays takes over that lane too; the isolated note ends at the same seam.
- An isolated note targeting a lane already owned by a global cue is rejected while the global cue runs.

At completion each affected lane **independently returns to its actual underlay**, evaluated per lane at `endAt`:

| Lane underlay at `endAt` | Rejoin target for that lane |
| --- | --- |
| Normal forward play | `timeline.positionAt(endAt)` — the hidden song frame |
| Global loop running | the global window's phase at `endAt`: `start + ((positionAt(endAt) − start) mod lengthS)` from `globalLoop` `:407` |
| That lane has its own lane loop | the lane-loop window's phase at `endAt`, the same hidden-pointer math as `scheduleLoopRelease` `:1147-1190` |
| Transport stopped / stopping | no rejoin voice; the cue voice is faded out and the lane is left silent |

A global cue can therefore end with four *different* rejoin targets — e.g. two lanes in the global loop phase, one in its own lane loop, one on the normal timeline. Rejoin error target: ≤ 2 frames per lane.

## 7. Precedence table

| Situation | Result |
| --- | --- |
| FUNCTION + Track(s) held + Note On | FUNCTION wins → global learn |
| Exactly one Track held + Note On | isolated learn on that lane |
| Two or more Tracks held + Note On | **rejected explicitly**, reason shown |
| No qualifier + learned note | one-shot playback |
| No qualifier + unlearned note | no-op, reported |
| Track hold that already produced `lane.audition` `:3979` | audition unaffected; the note adds only learning |
| FX overlay open | learning eligibility unchanged; playback allowed; FX controls untouched |
| Heads mode | learning and playback both rejected (Heads owns `headsBus` `:1802`) |
| Scrub / reverse / any loop active | learning rejected; playback allowed and rejoins per §6 |
| Same channel+note relearned | overwrite |
| Note On while that note plays | retrigger from `startFrame`, one bounded crossfade |
| Two global notes overlapping | last wins; the earlier ends at that seam |
| Global note over an isolated one | global takes all four lanes; isolated ends |
| Isolated note into a globally-owned lane | rejected while the global cue plays |
| Note Off during playback | ignored (one-shot) |
| All Notes Off / disconnect | open captures discarded; playing cues end at the next seam |

## 8. Persistence (additive, per project)

`StoredProject.control` gains one optional field, following `songGrid?` (`store.ts:76`):

```
cues?: {
  version: 1;
  markers: {
    id: string;
    channel: number;        // learning is keyed by channel+note
    note: number;
    scope: "global" | "lane";
    lane: 0|1|2|3 | null;
    startFrame: number;
    endFrame: number;
    sampleRate: number;
    /** Content identity of every stem the marker depends on. */
    sources: { role: string; contentHash: string; sourceGeneration: number }[];
    createdAt: number;
  }[];
}
```

- `contentHash` comes from `StoredStem.contentHash` (`store.ts:43`); `sourceGeneration` is a per-role counter bumped whenever a stem is (re)adopted in the session.
- **Invalidation**: on load and on any stem replacement, a marker whose recorded hash/generation no longer matches its stem is marked invalid — retained on disk, but unplayable and shown as "source replaced" in the status strip. A global marker is invalidated if *any* of the four stems changed; an isolated marker only by its own lane.
- Rate mismatch: frames are converted through seconds using the recorded `sampleRate`; markers past the song duration are clamped and flagged.
- `SCHEMA_VERSION` stays 2 (purely additive optional field). Absent → `{ version: 1, markers: [] }`.
- Privacy: metadata only, asserted by extending `src/audio/__tests__/privacy.test.ts`.

## 9. Normalized MIDI event contract

```
type StemMidiEvent = {
  kind: "noteOn" | "noteOff" | "allNotesOff";
  note: number; velocity: number; channel: number;   // channel 0..15
  timestampMs: number;        // performance.now() domain, monotonic
  source: "webmidi" | "coremidi-bridge" | "test";
  deviceId: string; deviceName: string;
};
```

- Desktop/Android: Web MIDI adapter converts status bytes + `DOMHighResTimeStamp`.
- iOS/iPadOS: the native bridge delivers **arrays** of these objects with the identical shape; the web layer cannot tell the transports apart.
- Note On with velocity 0 is normalized to `noteOff`. CC 123 → `allNotesOff`.
- Tests inject the same objects, so no hardware is needed.

## 10. iOS/iPadOS native wrapper (CoreMIDI)

Safari and Chrome on iPhone/iPad do **not** support Web MIDI (Chrome on iOS is WebKit). The native shell is the only MIDI path on those devices. Initial delivery: a TestFlight WKWebView shell loading the published Stem Tape URL; offline bundling later.

New `ios/` folder, outside the Vite build:

- `StemTapeApp.swift` — SwiftUI shell.
- `WebHostView.swift` — `WKWebView`, `allowsInlineMediaPlayback = true`, `mediaTypesRequiringUserActionForPlayback = []`, loads the published URL.
- `MidiBridge.swift` — `MIDIClientCreateWithBlock` + **one MIDI input port** (`MIDIInputPortCreateWithProtocol`) connected to every source with `MIDIPortConnectSource`. **No virtual destination is created** — nothing needs to advertise Stem Tape as a MIDI destination. Sources are enumerated at start and re-enumerated on `MIDIObjectAddRemoveNotification` (hot-plug).
- **MIDI 1.0 device support**: USB class-compliant controllers (Lightning/USB-C Camera Adapter or USB-C hub) and Bluetooth LE MIDI both appear to CoreMIDI as ordinary sources. The port is created with `MIDIProtocolID._1_0` so incoming MIDI 1.0 is delivered natively; if the port is negotiated to UMP, MIDI 1.0 channel-voice messages arrive as UMP message-type 2 and are decoded to the same note/channel/velocity fields. Either way the page sees identical `StemMidiEvent`s.
- BLE pairing uses the standard `CABTMIDICentralViewController` picker.
- **Delivery to the page**: events are **batched** per read block and sent with `webView.callAsyncJavaScript("window.__stemTapeMidi.push(events)", arguments: ["events": [[String: Any]]], in: nil, in: .page)` — **structured arguments only; JSON is never interpolated into an executable string.**
- **Clock anchoring**: `MIDITimeStamp` (mach absolute ticks) → seconds via `mach_timebase_info`, then mapped to the page's `performance.now()` origin by a calibration exchange at bridge-ready. The anchor is **re-established after foregrounding, after any `AVAudioSession` interruption, and after a route change**, because the WebKit process clock and the page's `performance.now()` origin can drift or reset.
- `AudioSessionController.swift` — `AVAudioSession` category `.playback`, `setActive(true)`; handles `interruptionNotification` (pause, then resume + re-anchor + re-unlock the `AudioContext`) and `routeChangeNotification`. Because the category is `.playback`, **the hardware Silent switch does not mute the native app.**
- Background/foreground: on background, emit `allNotesOff` into the page; on foreground, re-enumerate sources and re-anchor.

**Silent Mode disclosure applies to browser playback only.** In a browser on iPhone/iPad the page may be muted by the Silent switch and MIDI is unavailable; the status strip says so and points to the native app. The native shell shows no such warning.

## 11. Exact file changes

Web engine
- `src/audio/midi/contract.ts` (new) — `StemMidiEvent`, velocity-0 and CC-123 normalization.
- `src/audio/midi/clock.ts` (new) — `performance.now()` ↔ `AudioContext` calibration and `ctxTimeOf(ev)`.
- `src/audio/midi/webMidi.ts` (new) — Web MIDI adapter (desktop/Android).
- `src/audio/midi/nativeBridge.ts` (new) — `window.__stemTapeMidi` batched-array queue + bridge-ready handshake.
- `src/audio/cues.ts` (new) — marker store keyed by `channel:note`, learn/play state machines, eligibility gate, invalidation — all pure over `StemMidiEvent`.
- `src/audio/engine.ts` — add `cueOwner: (CueOwner|null)[]`; extend the existing identity protection so `fadeOutAndStop` `:845`/`isReleaseTarget` `:849` also spare a cue voice; add handlers `cue.learn.start`, `cue.learn.end`, `cue.play`, `cue.stopAll`, `cue.clear` that reuse `spawn` `:782`, `fadeOutAndStop` `:845` and the existing rejoin math; schedule completion + per-lane rejoin at play time; reschedule on rate change; `tick()` reaps only. **No change** to `releaseGlobalLoop` `:1074`, `scheduleLoopRelease` `:1147`, or `applyAudibility` `:1650`.
- `src/audio/commands.ts` — append the five command types to the union `:14-94`.

Input/qualifiers
- `src/device/useDeviceSurface.ts` — subscribe both adapters, read `functionHeld` `surface.ts:95` and held-Track lanes `surface.ts:324` at event time, dispatch cue commands. No gesture-engine change.

Persistence
- `src/audio/store.ts` — `control.cues?` type; `src/audio/session.ts:117` — write/restore, plus per-role `sourceGeneration` bumping on stem adoption.

UI status
- `src/device/CueStatus.tsx` (new), beside `HeadsStatus.tsx`: transport source (native bridge vs Web MIDI vs none), device name, armed scope, learned/invalid marker counts, last event, last rejection reason, and the browser-only Silent Mode + no-MIDI disclosure. No pads, no editor.

Native
- New `ios/` folder per §10; not part of the Vite build.

Guide
- `src/device/guideContent.ts` — feature id `stem.instrument.cues` and one lesson that animates FUNCTION-held global learning then a single-Track-held isolated learn on the SP-1 outline (existing `Control`s and motions only, `:250-260`), with body copy distinguishing **native iPhone/iPad MIDI (CoreMIDI app)** from **browser MIDI (desktop/Android Web MIDI)**. No drawn pads, no new SVG.
- Because `guideCoverage.test.ts:21-23` pins 20 lessons and `:44-47` pins 80 ids, the new lesson either replaces content inside an existing lesson slot or both counts are updated together in the same change — decided at implementation time and stated in the checkpoint.

## 12. Tests

Unit (vitest, injected `StemMidiEvent`s, `MockCtx` from `src/audio/__tests__/mockAudio.ts`)
- Timestamp conversion: an event delivered 120 ms late still resolves the frame at its own timestamp, ≤ 2 frames error.
- Learning: global, isolated, overwrite on same channel+note, interleaved overlapping captures on two keys stay independent, sub-minimum discard.
- Eligibility: learning rejected in Heads / scrub / reverse / global loop / lane loop / stopped, each with a distinct reason; a capture interrupted by a loop capture is discarded.
- Two Tracks held → explicit rejection, no marker written.
- Voice counts: at the seam ≤ 2 voices on an affected lane, after the seam exactly 1; unaffected lanes never change voice identity during an isolated cue.
- One-shot: Note Off during playback changes nothing; retrigger yields one bounded crossfade.
- Rejoin matrix: all four §6 underlays, per lane, ≤ 2 frames (tolerance style of `loopRejoin.test.ts:93`); one global-cue case where the four lanes have three different underlays.
- Scheduling: completion seam is scheduled at play time — a test that never calls `tick()` still observes the scheduled rejoin start.
- Invalidation: replacing one stem invalidates its isolated markers and all global markers, and leaves others playable.
- Persistence round-trip; privacy test asserts no audio bytes in markers.

Browser (Playwright, `tests/browser/`)
- Events injected via `window.__stemTapeMidi`: learn + play with the FX overlay open; `normalBus` RMS proves faders and FX process cue audio; live source-count probe proves no doubling outside the seam.

Physical device (TestFlight)
- USB class-compliant and BLE MIDI 1.0 controllers: hot-plug add/remove mid-performance; note-to-audio latency (target < 20 ms median); Silent switch ON must **not** mute the native app; call interruption then resume re-anchors the clock and audio; backgrounding leaves no stuck cue; a 30-minute run shows no clock drift beyond the 2-frame rejoin tolerance.

## 13. Implementation checkpoints (each independently reviewable)

1. Contract + clock calibration + both adapters + `CueStatus` strip. No audio behavior. Review: events and timestamps visible on screen.
2. `cues.ts` — marker store, keying, eligibility gate, invalidation. Pure, fully unit-tested, no engine change.
3. Engine: cue voice ownership with identity protection, **isolated cues only**, scheduled completion + rejoin for normal play. Review: voice-count and rejoin tests.
4. Rejoin matrix completed (global loop phase, lane loop phase, stopped) — still isolated cues.
5. Global cues: four-lane ownership, per-lane independent underlay return, precedence arbitration.
6. Persistence + migration + invalidation wired to stem replacement.
7. Guide card + coverage test update.
8. iOS wrapper: client + input port, batched `callAsyncJavaScript`, `AVAudioSession.playback`, re-anchoring; TestFlight build and the physical-device acceptance list.

## 14. Exclusions

No MIDI clock or output, no quantization, no launch modes, no cue editor, no onscreen pads, no fixed note bank, no new FX, no change to global-loop or lane-loop semantics, no Heads redesign, no unrelated UI, no offline bundling in the first iOS delivery, and no implementation in this response.

## 15. Open decision

Only one remains: whether the Guide's `stem.instrument.cues` lesson should **replace** an existing lesson slot (keeping the pinned 20-lesson / 80-feature counts) or **extend** the Guide to 21 lessons / 81 features with the coverage test updated. Everything else in this plan is settled.
