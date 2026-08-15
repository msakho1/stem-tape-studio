# Stem Tape — Firmware Contract v1

Contract version: **1.0.0**
Binding map version: **stem-tape-v1.0.0** (`src/machine/stemTapeV1Map.ts`)
Machine-readable companion: [`docs/firmware-contract-v1.json`](./firmware-contract-v1.json)

This document is the interface specification between the Stem Tape control surface
(hardware SP-1, or the browser digital twin) and the audio engine. It defines the
controls, the gesture rows, the arbitration rules, the timing constants, and the MIDI
event contract. It does not describe implementation.

The JSON companion is generated from the same binding-map source as the running app,
so the two cannot drift: 67 rows — tape 40, stem 4, heads 7, fx-overlay 10, system 6.

## 1. Principles

1. Audio truth lives in the engine. The surface emits an **ordered command stream**, never snapshot diffs.
2. The playhead is **derived** from the audio clock through the integrated rate curve. It is never ticked.
3. Global loop loops the **song timeline**; release continues from the audible frame. Hidden-timeline rejoining applies only to isolated stem loops and Heads.
4. Tempo, beat phase and downbeat detection are **automatic, local, deterministic and non-AI**. The musician is never asked to set a grid.
5. No user audio leaves the device. Memory figures are MiB (1024²) and labelled MiB.

## 2. Controls

| Class | Identifiers |
| --- | --- |
| Buttons | `play`, `record`, `function`, `track-button-1..4`, `volume-minus`, `volume-plus` |
| Rockers | `rocker-fwd`, `rocker-rev` |
| Faders | `fader-1..4` (continuous, multi-touch) |

Lanes are fixed and ordered: **1 Vocals · 2 Drums · 3 Bass · 4 Instruments**.

## 3. Arbitration

- **Longest chord wins.** A row whose control set is a strict superset of another row's set claims the input first.
- **Claim before dispatch.** A winning row's `suppresses` list is applied *before* any base command is dispatched, so qualifier buttons (`play`, `function`, `track-button-n`) never leak their solo behaviour.
- **Rollback.** Rows marked `txn-snapshot` capture engine state on entry and restore it if a longer chord supersedes them inside the discrimination window; all other rows are `none`.
- **External claims.** `claimExternal()` suppresses hardware gestures while a MIDI qualifier is held.

## 4. Timing constants (ms)

| Name | Value | Meaning |
| --- | --- | --- |
| `playTapHoldMs` | 450 | PLAY tap vs. hold (cue) boundary |
| `stemSoloLinkThresholdMs` | 700 | Solo vs. link, measured from two-control overlap |
| `fxOverlaySecondPressMs` | 120 | Second volume press window for the FX overlay |
| `fxOverlayReleaseMs` | 600 | Both volume keys must release inside this to toggle FX |
| `pairingHoldMs` | 2000 | Stock Bluetooth pairing gesture |
| `exportHoldMs` | 900 | FUNCTION + RECORD → WAV export |
| `antiClickFadeMs` | 8 | Anti-click fade on cue/stop |
| `cueSeamMs` | 12 | Maximum window in which two voices may overlap on one lane |
| `staleMidiEventMs` | 250 | Events older than this are reported stale |

The 600–2000 ms volume-chord band is an explicit **no-op** (diagnostics only), never an ambiguous action.

## 5. Layers

| Layer | Rows | Scope |
| --- | --- | --- |
| `tape` | 40 | Stock Tape Looper v2.6 rows plus the reinterpreted transport rows |
| `stem` | 4 | Stem selection, solo, phase-continuous link |
| `heads` | 7 | Heads Mode v2 — head N is lane N on its own clock |
| `fx-overlay` | 10 | Filter / echo / reverb momentary, variation, latch, plus clear-latches |
| `system` | 6 | FX overlay toggle, pairing, grid learn/quantise, WAV export |

### Reinterpreted v2.6 rows

| Row | v2.6 behaviour | Stem Tape behaviour |
| --- | --- | --- |
| `play.cue` | Hold PLAY restarts the loop from the top | Hold PLAY parks at frame 0 with an 8 ms fade and arms an exact launch |
| `rocker.scrub` | FUNCTION + rocker halves/doubles chop | FUNCTION + rocker is an audible four-stem shuttle on one shared playhead |
| `rocker.chop.play` | — | Chop moves to PLAY + rocker so FUNCTION + rocker can shuttle |
| `heads.lane.play` | Hold a loaded track to make it the head source | Hold a track to hear exactly that head, transport stopped or not |
| `heads.lane.latch` | Heads on = three tracks replay a quarter apart | Triple-tap latches independent per-head playback |
| `heads.lane.scrub` | FUNCTION + faders scrub the shared heads | FUNCTION + fader N scrubs head N audibly; release parks the loop start |

Every row's full `command`, `suppresses`, `supersedes`, `originalBehaviour`, `led` and
desktop key bindings are enumerated in the JSON companion.

## 6. MIDI contract (Stem Instrument Mode)

Cue identity is **`channel:note`**. Pads must transmit a unique note, a unique channel,
or a unique combination; pads sharing a note must be assigned different channels.

Accepted events: `noteOn`, `noteOff`, `allNotesOff` (CC 123).
Normalizations: `noteOn` velocity 0 → `noteOff`; CC 123 → `allNotesOff`; clock,
aftertouch, program change, sysex and all other CCs are dropped.

```ts
type StemMidiEvent = {
  kind: "noteOn" | "noteOff" | "allNotesOff";
  note: number;        // 0..127
  velocity: number;    // 0..127
  channel: number;     // 0..15
  timestampMs: number; // performance.now() domain, monotonic
  source: "webmidi" | "coremidi-bridge" | "test";
  deviceId: string;
  deviceName: string;
};
```

Transports: desktop/Android Web MIDI, and the iPhone/iPad CoreMIDI bridge
(`window.__stemTapeMidi.push(events)` with a native/page timestamp-anchor handshake).

### Cues

| Gesture | Result |
| --- | --- |
| FUNCTION + hold MIDI key | Learn a **global** cue over the held span (all four stems) |
| TRACK n + hold MIDI key | Learn an **isolated** cue on lane n |
| Hold a learned key | Play its cue; release returns |

- Global cue playback **parks** the song timeline and resumes at the saved frame on release.
- Isolated cue playback replaces one lane only; the other three lanes and the timeline keep running, and the lane rejoins the others' current position on release.
- Cues never loop: if the passage ends before release, the cue returns automatically.
- Minimum capture length: **1024 frames**. Markers are invalidated when the owning lane's `contentHash` changes.
- Learning is rejected until the relevant stems are decoded (all four for global, the target lane for isolated), during heads/scrub/reverse states.
- Mixer, loop, FX and transport state are preserved across every cue.

## 7. Change policy

Any change to a row id, control set, threshold, suppression list, or the MIDI event
shape is a **breaking** change: bump `contractVersion` and regenerate
`docs/firmware-contract-v1.json` from the binding map.
