# Stem Tape — Phase 1 (revised): SP-1 Control-Surface Digital Twin

New scope: a functional interaction prototype of the SP-1 control surface, with no Web Audio engine. Everything below serves one question — can the stock hardware controls carry both stock stem-player behaviour and the proposed four-track tape functions, and can LEDs alone communicate the resulting state?

Labelled **"Unofficial Stem Tape Prototype"** throughout. Layout inspired by the physical control arrangement; no TE logos, wordmarks or product renders.

## Gesture vocabulary taken from the supplied KE guide (rev.02)

Transcribed from the uploaded manual page and treated as the stock baseline the FSM must reproduce:

- Hold **function** ~2 s to power on/off; press **play** to confirm master volume; press play to start/pause.
- Four device modes: basic; normal; normal + PO sync out; normal + MIDI thru / capture. Enter with **function + play** held ~2 s — four tap LEDs flash — then **volume +/-** to select, **play** to confirm.
- Pairing: hold **volume + and volume -** together ~2 s to scan; side LEDs indicate scanning. Side LEDs also show battery on power-up.
- **Rocker**: fwd / neutral / rev for fast-forward and rewind. **Function + rocker** skips song forward/back. While the rocker is engaged, **volume +/-** selects playback speed.
- **Faders** set per-stem volume.
- **Track selection**: short click on function enters track select — top LEDs pulse — then **volume +/-** steps the active track, solid top LED = selected. **Function + track buttons** solos tracks.
- **Effects**: press a track button *without* function to activate that track's effect; four variations each, chosen by holding the track button and using **volume +/-**. Printed labels read `rev`, `pitch`(?), `delay`(?), and a fourth that is not legible in the scan.
- **Loop**: press and hold **play** to activate loop; loop mode shown on side LEDs. While holding play, **volume** sets loop fade-in and the **rocker** moves the loop. Loop is indicated while play is held; when not latched and not looping, **function + play + volume** sets the loop divider.
- **Latch**: a short press on **function** while holding a control makes effects, loop and fwd/rew sticky. Press play to unlatch loop; use the rocker to unlatch fwd/rew. A short function click while holding a latched fx button un-latches fx on the active track; holding all track buttons while pressing function clears all fx latches.
- **Sync**: mode 3 for Pocket Operator sync (PO clock 1-5-7) via the sync jack; mode 4 for MIDI clock via the 3.5 mm jack.

**Uncertain items, surfaced in-app as `VERIFY ON HARDWARE` rather than invented:** the fourth effect label (illegible in the scan), the exact `pitch`/`delay` spellings, loop-divider value set, and whether multiple simultaneous solos are supported. Each renders with a distinct warning treatment and appears in an "Open questions" list on the diagnostics panel.

## Architecture — FSM first, React second

All interaction logic lives in a framework-free machine under `src/machine/`. React only dispatches input events and renders derived output. No gesture logic in any component.

```text
pointerdown/up/cancel, keydown/up
        │  (normalized ControlEvent: {control, phase, t})
        ▼
  GestureRecognizer ──uses──► MappingRegistry (editable in the Mapping Lab)
        │  emits semantic Intents: TOGGLE_PLAY, ENTER_TRACK_SELECT,
        │  SOLO_TRACK(n), SET_FX_VARIATION(n,v), LATCH(target),
        │  MOVE_LOOP(dir), REVERSE_TRACK(n), ARM_RECORD(n), UNLINK(n)…
        ▼
  StemTapeMachine (pure reducer: DeviceState × Intent → DeviceState)
        │
        ├─► LedRenderer   (DeviceState → LedFrame: top×4, side×n, colour/blink phase)
        ├─► SimClock      (rAF tick → positions, meters, waveform scroll; no audio)
        └─► Diagnostics   (DeviceState → human-readable explanation of every LED)
```

- `ControlEvent` normalizes pointer and keyboard input, with pointer capture so a press survives dragging off the button, and `pointercancel` release safety.
- `GestureRecognizer` resolves short press (<250 ms), long press (configurable, default 600 ms), hold-plus-modifier, double press (<300 ms gap), latched hold, and multi-button chords (all-down within a 120 ms window). Timers are virtual so the demo script and tests can run faster than real time.
- The reducer is pure and serializable — every state transition is recordable, replayable and testable without a DOM.

## State model

```ts
type ControlId = 'play'|'function'|'volUp'|'volDown'|'track1'|'track2'|'track3'|'track4'|'rockerFwd'|'rockerRev';
type DeviceMode = 'basic'|'normal'|'normal_po_sync'|'normal_midi_capture';
type PrototypeMode = 'stock'|'tape_looper'|'stem_tape';   // dev-only selector, outside the device
type StemRole = 'vocals'|'drums'|'bass'|'instruments';
type FxSlot = 'rev'|'pitch'|'delay'|'fx4_unverified';

interface LoopRegion { startBeats: number; lengthBeats: number; enabled: boolean; latched: boolean; divider: number; fadeIn: number }
interface TrackState {
  role: StemRole; fader: number; muted: boolean; soloed: boolean;
  fx: { slot: FxSlot; variation: 0|1|2|3; active: boolean; latched: boolean } | null;
  loop: LoopRegion | null;            // null = follows sharedLoop
  linked: boolean; direction: 1|-1; rate: number; positionBeats: number;
  record: 'idle'|'armed'|'recording'|'overdub'; saturation: number;
}
interface DeviceState {
  power: 'off'|'booting'|'on'|'pairing';
  deviceMode: DeviceMode; prototypeMode: PrototypeMode;
  transport: { playing: boolean; positionBeats: number; direction: 1|-1; rate: number; bpm: number };
  sharedLoop: LoopRegion;
  held: Partial<Record<ControlId, number>>;   // control -> pressed-at timestamp
  functionModifier: boolean; latchArmed: boolean;
  uiFlow: 'idle'|'track_select'|'mode_select'|'loop_edit'|'fx_variation'|'pairing';
  activeTrack: 0|1|2|3; soloSet: number[];
  tracks: [TrackState, TrackState, TrackState, TrackState];
  songIndex: number; songs: { name: string; bpm: number; bars: number }[];
  leds: LedFrame; lastGesture: string | null; conflicts: MappingConflict[];
}
```

## Control Mapping Lab

A panel beside the device. Every proposed Stem Tape action (reverse track, unlink, relink, arm record, overdub, clear track, per-track varispeed, per-track loop shorten/lengthen, saturation, song switch, live record into slot) is a row where you pick a **trigger type** (short / long / function+ / double / latched hold / chord) plus its **control(s)**.

- Changes apply to the live machine immediately — the registry is the recognizer's only source of truth, so there is nothing to recompile or reload.
- **Conflict detection** compares each proposed binding against a frozen table of the stock gestures transcribed above, flagging exact collisions (same control + same trigger), shadowing (a long press on a control whose short press is stock — usable but reported), and chord subsets. Conflicts render inline on the row and aggregate in diagnostics.
- Mappings are named presets, saved to localStorage, exportable as JSON — that export is the interaction specification handed to the audio-engine phase.

## Visual and simulated feedback

- **Device**: four vertical faders (pointer-drag, keyboard-accessible), four track buttons, volume +/-, play, function, a three-position side rocker (drag or hold up/down, spring return to neutral unless latched), top LED row ×4, side LED strip, and USB/sync indicators.
- **LEDs** are the device's only native output: solid, pulse, flash-n-times, chase and breathe patterns, driven from `LedFrame` on a single rAF loop so blink phases stay coherent.
- **Waveforms**: four procedurally generated static waveform images per song (deterministic PRNG, role-appropriate envelopes — drums spiky, bass smooth), with a shared timeline plus four compact per-track lanes showing that track's playhead, loop region, direction arrow and rate. All motion comes from `SimClock`, not audio.
- **Meters** are simulated from fader × envelope × mute/solo state.
- **Diagnostics panel** (toggleable) lists every item requested: mode, active stem, solo set, active/latched effects and variations, loop state and length, record/overdub, link state, direction and rate, currently held controls, last recognized gesture, and mapping conflicts — each LED given a plain-language reading ("top LED 2 pulsing = track 2 selected, awaiting volume +/-").
- **Demo mode**: a scripted timeline of ControlEvents fed into the same input path as real presses — it presses the actual on-screen controls, so it demonstrates a full Stem Tape performance (sync start → shared 4-bar loop → drums to 1 beat → bass to 3 beats → reverse vocals → unlink instruments → fader arrangement) and simultaneously acts as an end-to-end test. Play/pause/step/speed controls, with a caption track explaining each gesture.

## Files

```text
src/machine/    types.ts  machine.ts  gestures.ts  mappings.ts  stockGestures.ts
                conflicts.ts  leds.ts  simClock.ts  demoScript.ts
src/components/device/   DeviceShell  Fader  TrackButton  PlayButton  FunctionButton
                         VolumeButtons  Rocker  LedRow  SideLeds  StatusIndicators
src/components/panels/   PrototypeModeSelector  MappingLab  DiagnosticsPanel
                         WaveformTimeline  TrackLane  DemoController  OpenQuestions
src/hooks/      useDeviceMachine.ts  usePressGesture.ts
src/routes/     index.tsx (the prototype)  about.tsx (scope, sources, disclaimer)
```

Single React store holding machine state, updated by dispatch; components subscribe by selector so a fader drag doesn't re-render the whole surface. Pointer events use `setPointerCapture`; keyboard equivalents (space = play, F = function, 1-4 = tracks, arrows = volume/rocker) hold and release properly so combinations are reachable without a mouse.

## Verification

Machine-level tests (no DOM): each stock gesture from the manual produces the documented state change; hold thresholds; chord windows; latch/unlatch paths including "hold all four track buttons + function clears all fx latches"; conflict detection against known-colliding and known-safe bindings; the demo script runs to completion with the expected end state. Manual pass: hold-and-combine works with mouse and with touch on a tablet-sized viewport.

## Explicitly not in this phase

Web Audio, real files, recording/export, persistence beyond mapping presets and session state, PWA. The output of this phase is the exported interaction specification plus answers to the five questions posed.
