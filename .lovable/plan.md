# Stem Instrument Mode — MIDI cue learning, iOS CoreMIDI wrapper, Guide card

Plan only. No code in this response.

## 1. Current-code audit (verified this turn)

MIDI: **none exists.** `rg -ni "midi" src public` returns zero hits (only `webkitAudioContext` at `src/audio/engine.ts:465` and `-webkit-` rules in `src/styles.css:181-182`). There is no Web MIDI code, no permission prompt, no device list, no native wrapper, no Xcode project. Everything below is new surface area.

Reusable engine paths (all in `src/audio/engine.ts`, 4251 lines):

- Voice creation: `private spawn(t, startAt, offset, fadeIn)` at `:782`. Creates a `AudioBufferSourceNode` + per-voice `fade` gain, connects `fade → t.stemGate` (`:788`), applies reverse mirroring and loop wrap (`:793-801`), equal-power fade-in via `sampleCurve(equalPower,"b")` (`:812-815`), pushes to `t.sources`. This is the single correct way to add a voice on a lane.
- Voice removal: `private fadeOutAndStop(t, live, at)` at `:845`, with identity protection for a queued release target (`isReleaseTarget`, `:849`).
- Lane relocation without touching the shared clock: `respawnLane` `:828`, `relocateLane` `:1021`. Shared-clock relocation anchors at `:1011`.
- Shared clock: `this.timeline.positionAt(...)` / `this.timeline.anchor(at, pos)` (`:903, :1011, :1137, :3033`); `position()` at `:752-758`.
- Global loop: state `globalLoop: { start; lengthS; division } | null` at `:407`; `loop.global.start` handler at `:4049`; release `releaseGlobalLoop()` at `:1074` — captures the audible frame (`:1101`), closes windows, spawns straight sources, anchors the shared timeline to the audible landing (`:1137`). **Song-timeline semantics; no hidden rejoin.** Unchanged by this work.
- Lane loop: `loop.capture` / `loop.release` / `loop.resize` at `:3990-3998`; hidden-timeline bar-boundary rejoin in `scheduleLoopRelease` at `:1147-1190` (`hiddenNow = timeline.positionAt(now)` `:1162`). Unchanged.
- Audibility/ownership: `applyAudibility(id)` at `:1650` — solo/mute/audition resolution, then `fxRack.setInputOpen(stemAudible)` and `setGain(t.stemGate.gain, …)` (`:1665-1667`). This is the gate a cue must borrow to silence the voice it replaces.
- FX: per-lane rack created ahead of `stemGate` (`stemGate.connect(input)` `:529-531`); buses `normalBus` `:302/:509` and `headsBus` `:1802/:516`, taps and RMS at `:1891-1896`. Any voice connected to `t.stemGate` is automatically processed by that lane's fader, FX rack and the normal bus — so cue voices reuse it verbatim.
- Momentary audition: `case "lane.audition"` `:3979` → `setAudition(mask)`.
- Lane fader scrub: `laneFaderScrub` `:2846-2968` (park/candidate model).
- Heads: separate bus and voices, `spawnHeadVoice` `:1970`, `heads.enter` `:3916`.

Command stream: `src/audio/commands.ts` — `AudioCommandType` union (`:14-94`), `AudioCommand { id, t, type, payload }` (`:99`), `makeCommand` (`:128`), ack model (`:112-120`). Additive by design.

Input/state: `src/input/gestures.ts` (496 lines, deferred arbitration), `src/machine/chordArbiter.ts` (533 lines, first-claim PLAY ownership), `src/machine/surface.ts` (1397 lines) — `functionHeld: boolean` at `:95`, held-Track derivation `state.pressed.filter(x => x.startsWith("track-button")).map(trackIndexOf)` at `:324` and `:746`, `auditionChord` `:169`, `headsMode` / `perf.fxOverlay` referenced at `:747`. Host: `src/device/useDeviceSurface.ts` (801 lines) owns the pointer/keyboard event source and engine wiring (`getAudioEngine()` `:30`).

Persistence: `src/audio/store.ts` — `SCHEMA_VERSION = 2` `:10`, `StoredProject.control { faders, mutes, masterVolume, speed, globalLoop, chopDiv?, window, filter, grid, songGrid?, song }` `:52-95`; IndexedDB stores `projects`/`blobs` `:15-16`. `src/audio/session.ts:117 toStoredProject()` is the single writer. Precedent for additive optional fields: `songGrid?` and `chopDiv?`.

Guide: `src/device/guideContent.ts` (485 lines) — `FEATURE_IDS` model, `PERFORMANCE` list `:44-81` (37 ids), `Lesson { highlight: Control[]; motion: MiniMotion; held?: Control[] }` `:250-260`, 20 lessons `:265+`. Renderer `src/device/ControlsGuide.tsx` maps `LESSONS` → `Sp1GuideIllustration` (`:87-101`). Illustration `src/device/Sp1GuideIllustration.tsx` (190 lines) drives the authoritative `src/assets/stem-tape-sp1-outline.svg` via `data-active-controls`; motions are CSS in `src/styles.css`. Coverage test `src/device/__tests__/guideCoverage.test.ts`.

## 2. Concept

A MIDI key is a **cue marker**, not a pad. There is no instrument-mode toggle, no pad grid, no cue editor, no fixed note range: any note number can be learned, and unlearned notes do nothing.

- Hold FUNCTION + press a MIDI key → learn a **global** cue (all four stems).
- Hold Track N + press a MIDI key → learn an **isolated** cue for lane N.
- Note On starts the marker (`startFrame` = current song position frame); Note Off ends it (`endFrame`). Learning the same note again overwrites it.
- With no qualifier held, Note On **plays** the saved passage; Note Off ends it early.
- Only marker metadata is saved. No audio is copied.

## 3. Cue-learning state machine

States: `idle → armed(scope) → capturing(note, scope) → committed`.

- `armed` is entered by qualifier presence at the moment of Note On, read from the live surface state (`functionHeld` `surface.ts:95`; held lanes derived as at `surface.ts:324`). FUNCTION wins over Track holds (see precedence).
- On Note On while armed: `startFrame = round(engine.position() * sampleRate)`, `scope = global | lane N`, `sourceRole` recorded for isolated cues.
- On Note Off for that note: `endFrame`, commit if `endFrame - startFrame >= minCueFrames` (proposed 1024 frames ≈ 23 ms) else discard with a status message.
- Qualifier released mid-capture: capture continues to Note Off (the gesture is defined by the note, not the button).
- Same note learned again: overwrite in place, keeping id and scope from the new capture.
- Transport stopped while learning: allowed; frames come from `position()` which is valid when parked (`engine.ts:757`).

## 4. Cue-playback state machine

Per note: `idle → playing(voice set) → ending → idle`.

On Note On for a learned cue with no qualifier held:

- **Global cue**: mute the normal mix path for all four lanes at the gate (`applyAudibility` path, `engine.ts:1650-1667`) and spawn one cue voice per lane at `startFrame` via `spawn(t, at, startPos, true)` (`:782`). The normal voices are faded out with `fadeOutAndStop` (`:845`) — never left running underneath.
- **Isolated cue (lane N)**: only lane N's normal voice is faded out and replaced; lanes ≠ N are untouched and keep playing.
- Because cue voices connect to `t.stemGate` (`:788`), the lane fader, mute-state gate and full FX rack process cue audio with no new routing.

Ending: on Note Off, or when the cue voice reaches `endFrame` (scheduled seam, checked in the existing `tick()` sweep), the engine rejoins:

| Current state at rejoin | Global cue rejoin | Isolated cue rejoin |
| --- | --- | --- |
| Normal play | crossfade cue → new voices at `timeline.positionAt(at)` for all four lanes | crossfade lane N → `timeline.positionAt(at)` (hidden clock kept running) |
| Global loop active | rejoin at the shared loop-phase frame derived from the running global window (`globalLoop` `:407`), all four lanes | same, for lane N only |
| Lane loop active on that lane | n/a for other lanes | rejoin inside the lane's loop window at the phase the hidden pointer reached (same math as `scheduleLoopRelease` `:1147`) |
| Transport stopped | cue plays, then silence; no voices left | same, lane N only |

Invariant: **exactly one audible voice per lane at all times** — cue or source, never both. Enforced by asserting `t.sources.filter(live).length === 1` after every seam in tests.

## 5. Precedence table

| Situation | Result |
| --- | --- |
| FUNCTION held + Track held + Note On | FUNCTION wins → global learn |
| Track held (one or more) + Note On | isolated learn on the **lowest-indexed** held lane (multi-lane learn is out of scope) |
| No qualifier + learned note | playback |
| No qualifier + unlearned note | no-op, reported in the status strip |
| Track hold that also produced `lane.audition` (`engine.ts:3979`) | audition continues normally; the note only adds learning — no mute/loop side effect from the note |
| FX Overlay open (`perf.fxOverlay`) | learning and playback both work; FX controls unchanged |
| Heads mode active | notes are ignored (reported); Heads owns its own bus (`headsBus` `:1802`) |
| Global loop running | learn and play allowed; rejoin per §4 table |
| Lane loop running | learn and play allowed; isolated cue owns the lane until release |
| Same note re-armed | overwrite |
| Note On repeated while already playing | retrigger from `startFrame` with one crossfade; still one voice |
| Two different notes overlapping, both global | last note wins; the earlier one is ended with a seam |
| Global note while an isolated note is playing | global takes ownership of all four lanes; the isolated note ends |
| Isolated note on a lane already owned by a global cue | rejected while the global cue plays |
| Cancel | a learning capture is cancelled by All Notes Off / device disconnect / transport song change |

## 6. Persistence (additive)

`StoredProject.control` gains one optional field — same pattern as `songGrid?` (`store.ts:76`):

```
cues?: {
  version: 1;
  markers: {
    id: string;
    note: number;          // 0..127
    channel: number | null; // null = omni
    scope: "global" | "lane";
    lane: 0|1|2|3 | null;
    startFrame: number;
    endFrame: number;
    sampleRate: number;     // frames are validated against this
    createdAt: number;
  }[];
}
```

Migration: absent → `{ version: 1, markers: [] }`. `SCHEMA_VERSION` stays 2 (additive optional, consistent with `chopDiv?`/`songGrid?`). On load, markers whose `sampleRate` differs from the decoded context rate are converted by seconds, and markers beyond song duration are clamped and flagged. Written by `toStoredProject` (`session.ts:117`). No audio bytes stored — asserted by extending `src/audio/__tests__/privacy.test.ts`.

## 7. Normalized MIDI event contract

One shared type consumed by all transports:

```
type StemMidiEvent = {
  kind: "noteOn" | "noteOff" | "allNotesOff";
  note: number; velocity: number; channel: number;
  timestampMs: number;          // performance.now()-domain, monotonic
  source: "webmidi" | "coremidi-bridge" | "test";
  deviceId: string; deviceName: string;
};
```

- Desktop/Android: Web MIDI adapter converts raw bytes and DOMHighResTimeStamp into this shape.
- iOS/iPadOS: the native wrapper posts the same JSON objects into the page; the web layer cannot tell the difference.
- Test harness injects the same objects directly, so all unit tests run with no MIDI hardware.
- Velocity 0 Note On is normalized to `noteOff`.

## 8. iOS/iPadOS native wrapper (CoreMIDI, not Web MIDI)

Safari and Chrome on iPhone/iPad **do not support Web MIDI** (Chrome on iOS is WebKit). The only MIDI path is a native app.

Architecture (Xcode-ready, new `ios/` folder outside the web build):

- `StemTapeApp.swift` — SwiftUI app shell.
- `WebHostView.swift` — `WKWebView` with `configuration.allowsInlineMediaPlayback = true`, `mediaTypesRequiringUserActionForPlayback = []`, loading either the published URL or a bundled copy.
- `MidiBridge.swift` — `MIDIClientCreateWithBlock`, virtual destination + `MIDIPortConnectSource` for every source; `MIDINotification` for hot-plug add/remove (USB Class Compliant via Camera Adapter, and Bluetooth LE MIDI); `MIDIEventList` parsing (UMP), converting `MIDITimeStamp` (mach ticks) to a millisecond value aligned to the page's `performance.now()` origin via a one-time offset exchange.
- `WKScriptMessageHandler` in both directions: native → web with `evaluateJavaScript("window.__stemTapeMidi.push(<json>)")`, web → native for "request device list" and "bridge ready".
- `BluetoothMidiViewController` — the standard CoreAudioKit BLE MIDI picker.
- `AudioSessionController.swift` — `AVAudioSession` category `.playback`, `setActive(true)`, handling `AVAudioSession.interruptionNotification` (pause + resume) and route change; posts `interruption` events into the page so the engine can re-unlock the `AudioContext`.
- Background/foreground: on `didEnterBackground`, mark all notes off; on foreground, re-sync the device list.

Browser fallback disclosure: on any WebKit browser without `navigator.requestMIDIAccess`, the status strip states plainly that MIDI requires the native app and that audio still plays normally in the browser.

## 9. Smallest file changes

Web engine
- `src/audio/cues.ts` (new) — marker store, learn/play state machines, pure functions over `StemMidiEvent`.
- `src/audio/engine.ts` — add `cue.learn.start/end`, `cue.play.start`, `cue.play.end`, `cue.clear` handlers that reuse `spawn` (`:782`), `fadeOutAndStop` (`:845`), `applyAudibility` (`:1650`) and the existing rejoin math; add a `cueOwners: (null|"global"|"lane")[]` field and one branch in `tick()` for `endFrame` seams. No change to `releaseGlobalLoop` (`:1074`) or `scheduleLoopRelease` (`:1147`).
- `src/audio/commands.ts` — append the five command types to the union (`:14-94`).
- `src/audio/midi/contract.ts` (new) — `StemMidiEvent`.
- `src/audio/midi/webMidi.ts` (new) — Web MIDI adapter.
- `src/audio/midi/nativeBridge.ts` (new) — `window.__stemTapeMidi` queue adapter.

Input/qualifiers
- `src/device/useDeviceSurface.ts` — subscribe the MIDI adapters, read `functionHeld` / held Track lanes from surface state, dispatch cue commands. No gesture-engine change.

Persistence
- `src/audio/store.ts` — `control.cues?` type; `src/audio/session.ts` — write/restore it in `toStoredProject` (`:117`).

UI status
- Small `src/device/CueStatus.tsx` (new), placed next to `HeadsStatus.tsx`: device name, armed scope, learned-note count, last event, and the iOS fallback disclosure. No pads, no editor.

Native
- New `ios/` folder as described in §8. Not part of the Vite build.

Guide
- `src/device/guideContent.ts` — one feature id `stem.instrument.cues` plus one lesson using `highlight: ["function", "track-button-1"]`, `held: ["function"]`, `motion: "sequence"` (existing motions only — see `guideContent.ts:250-260`, `:304-336`). The card animates FUNCTION-held global learning, then a Track-held isolated learn, on the faithful SP-1 outline. No drawn pads, no new SVG.
- `src/device/__tests__/guideCoverage.test.ts` — id added to the coverage set.

## 10. Tests

Unit (vitest, injected `StemMidiEvent`s, `MockCtx` from `src/audio/__tests__/mockAudio.ts`)
- learn global / learn isolated / overwrite same note / discard sub-minimum length.
- global cue play: exactly 4 spawned voices, 0 surviving source voices per lane; isolated: 1 replaced lane, 3 untouched.
- rejoin error ≤ 2 frames for each of the four states in §4 (same tolerance style as `loopRejoin.test.ts:93`).
- precedence table row-by-row, including Heads rejection and FX-overlay pass-through.
- persistence round-trip; privacy test asserts no audio bytes in the marker payload.

Browser (Playwright, `tests/browser/`)
- injected events through `window.__stemTapeMidi`: learn + play with FX overlay open; measured RMS on `normalBus` proves FX and faders process cue audio; voice-count probe proves no doubling.

Physical device
- iPhone + USB class-compliant controller and BLE controller: hot-plug add/remove mid-performance, note-to-audio latency measured with a scope/recording (target < 20 ms median), interruption (incoming call) recovery, backgrounding leaves no stuck cue.

## 11. Implementation sequence (reviewable checkpoints)

1. Contract + adapters + status strip, no audio behavior. Reviewable: events visible on screen.
2. `cues.ts` state machines + unit tests. No engine change.
3. Engine cue voices for **isolated** cues only, with rejoin tests.
4. Global cues + ownership arbitration + precedence tests.
5. Persistence + migration.
6. Guide card + coverage test.
7. iOS wrapper project and physical-device acceptance.

## 12. Exclusions

No MIDI clock or MIDI output, no quantization menus, no launch modes, no new FX, no change to global-loop or lane-loop semantics, no Heads redesign, no unrelated UI, no onscreen pads or cue editor, no implementation in this response.

## 13. Decisions I need from you

1. **Multi-lane isolated learn.** Two Track buttons held at Note On: plan currently learns the lowest-indexed lane only. Alternative: learn one marker per held lane sharing the note.
2. **Cue length beyond Note Off.** Plan ends playback at Note Off *or* `endFrame`, whichever comes first. Alternative: Note Off always plays the full learned passage (latched) unless retriggered.
3. **Where cues live.** Plan stores markers per project. Alternative: a global cue bank shared across projects, keyed by note.
4. **iOS delivery.** TestFlight/App Store app loading the published URL, or a fully bundled offline build inside the app?
