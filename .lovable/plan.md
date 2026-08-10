# Stem Tape — Diagnostic Findings and Repair Plan

## What the live diagnostic actually showed

I ran the app in a real browser and drove it through the diagnostics bridge (`window.__stemTape`).

Loading the demo, then issuing the exact commands the SP-1 gestures emit:

```json
{ "type": "heads.enter",   "status": "rejected", "detail": "audio not unlocked" }
{ "type": "heads.play.hold","status": "rejected", "detail": "heads mode is not active" }
{ "type": "tape.reverse",  "status": "rejected", "detail": "audio not unlocked — enable audio, then repeat the gesture" }
```

Engine state at that moment: `ctx: null`, `tracks: []`.

**Root cause of "Heads mode does not work" and "Function + double-tap reverse does not work":** they are not broken features. Every audio command is gated behind an unlocked `AudioContext`, and the context is only created by the explicit "enable audio" button (or an ingest). If a user has not pressed that box — or the demo/stem load did not complete — the whole command stream is rejected silently to the ear. `lane.reverse` correctly re-dispatches to `tape.reverse`; it never gets to run.

This is the same root cause as "I have to click a file 2 or 3 times to load it": ingest calls `engine.unlock()` itself, so the first pick spends its click creating the context and, if that resolve races the picker, the pick appears to do nothing.

## The fix, in order

### 1. Audio is always on (removes three bugs at once)
- Delete the "enable audio" toggle from the tape page.
- Create and resume the `AudioContext` on the first pointer/key event anywhere in the app (a single global unlock listener installed at mount), and again on every ingest, transport press and mode entry.
- Any command that currently rejects with "audio not unlocked" instead awaits the unlock and then runs. Heads mode, FN + double-tap reverse, and per-lane scrub all start working without any further change to their logic.
- Show the running sample rate as passive text in the status rail instead of a button.

### 2. Stem loading
- One click loads one stem: reset the file input value after each pick, unlock before opening the picker, and keep the busy state per-cell so a second click is never needed.
- Fix "load all": ensure the picker is `multiple`, assign the picked files to roles in order, run the shared sequential ingest, then run grid analysis and refresh the cells (the per-cell path already does this; the all-path skips the analysis and refresh ordering).
- Verify by loading four files in one action and confirming four decoded cells.

### 3. Tempo display
- The tempo tile shows nothing (an em dash) until a stem is decoded and analysis has produced a real grid. No provisional 120.

### 4. Session naming
- The session name becomes an editable field in the project header; renaming persists with the project and replaces "untitled session" everywhere it is displayed.

### 5. Memory
- High memory is the default and the only mode. Remove the memory-mode selector and the local-storage drawer that only reports "off".

### 6. "What just happened" readout
- The readout currently prints the raw intent (`arbitrated → fx.momentary.end`). Replace it with a human sentence resolved from the command that was emitted: the FX name and bank for FX presses ("MOTION · Reel Flange — momentary, released"), the head and its action in Heads Mode ("Head 2 latched — playing independently at 4.21 s"), the lane and value for faders, and so on. One label table, used by both the tape page and the guide.

### 7. Controls and Guide
- "Show controls" becomes two independent things: a hit-zone overlay toggle (geometry only), and a control reference made of accordions — "how to scrub", "how to reverse a stem", "how to capture a loop", "heads mode", "FX banks", each drawer opening to show the button/gesture diagram plus a short looping animation of that gesture on the SP-1 illustration.
- The Guide tab is enriched with the same accordion content plus a short "first five minutes" walkthrough.

### 8. Header and footer copy
- Under "A four-track tape looper for the browser": "created by Mounir Sakho", linking to the Instagram profile.
- Remove the "No network request in this app ever contains your audio…" sentence.
- Remove the report download from the System page.
- Add a "support this project" button at the right of the unofficial/independent line, with a cupped-hand-and-floating-coin icon. Not wired to a payment provider yet — it opens a placeholder panel.

## Technical notes

- Global unlock: an `AudioEngine.ensureUnlocked()` awaited inside the command executor rather than a boolean check, so no gesture is ever lost to a cold context.
- The FX/heads labelling table lives next to the command definitions so the readout can never drift from the emitted command.
- No changes to grid analysis, the tape kernel, or the scrub handoff math.

## Verification

- Browser proof: cold load, immediately press Play + Track (heads), then FN + double-tap Track 1, and show non-rejected acks and heads RMS above zero with no "enable audio" click anywhere.
- Single-click load of one stem and a four-file "load all", with decoded byte counts and a detected BPM.
- Full unit suite must stay green.
