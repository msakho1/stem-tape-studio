# Stem Tape — Corrective Diagnostic and Repair Plan (v2)

Your critique is accepted. The previous plan tested a locked, empty engine through the
diagnostics bridge, which proves nothing about gesture recognition, tap arbitration or
routing from the rendered SP-1. Everything below is driven from the rendered surface with
real decoded stems, and no conclusion is reported without a command + acknowledgement pair
and audible evidence.

## Step 0 — Establish the two preconditions separately

The earlier probe conflated two failures. They are now separate, independently reported gates:

- **P-A: decoded audio.** Four stems ingested and reported decoded by the engine, with byte
  counts and a detected grid.
- **P-B: running context.** `ctx.state === "running"`, reached through a real trusted pointer
  event on the surface, never through a script call.

No Heads or reverse verdict is issued until both are green and printed.

## Step 1 — Trace the real file-input events (no speculation)

Instrument nothing; observe. Record, in order, for one stem cell:
`pointerdown` on "select" → `click` dispatched to the hidden `input[type=file]` → `change`
event with `files.length` → ingest result → refresh. Repeat for:

1. first selection into an empty cell
2. selecting the **same file again** (the classic no-`change` case: input value not reset)
3. replacing an already-loaded cell
4. WebKit, which fires the picker differently

Only after that trace names the missing event do I change code. The unlock must **not** be
awaited before `input.click()` — that would consume or delay the trusted activation. Unlock
is attached as a passive listener on the same gesture and resolved in parallel.

## Step 2 — "load all" = one picker, with a confirmed mapping before ingest

Today the markup already declares a multi-file picker:

```tsx
<input ref={allInput} type="file" multiple ... onChange={(e) => void onPickAll(e.target.files)} />
<button onClick={() => allInput.current?.click()}> ↑ load all </button>
```

and `onPickAll` assigns roles blindly by array index
(`role: STEM_ROLE_LIST[i]`), which is exactly how drums silently land in vocals.

Repair (option a), with a mandatory confirmation stage:

1. One picker, up to four files.
2. A mapping panel appears: each chosen filename beside a role selector
   (vocals / drums / bass / instruments), pre-filled by a filename heuristic but
   fully editable, duplicates blocked, unassigned files blocked.
3. Nothing is decoded until the user confirms the mapping. Confirm runs the same
   sequential ingest path, then grid analysis, then a cell refresh.
4. Cancel discards without touching the engine.


## Step 3 — Audio always on

Delete the "enable audio" toggle. The context is created and resumed from the first trusted
interaction anywhere in the app, and any command arriving at a cold context awaits that unlock
instead of returning `rejected: audio not unlocked`. Sample rate becomes passive status text.

## Step 4 — Comprehensive Heads audit, on the rendered surface

Entry is **Function + triple-tap Play**, performed as pointer events on the SVG. Each item
below is a separate pass that records: the gesture, the emitted command, the engine
acknowledgement, the heads-bus RMS, the head positions, and the readout sentence.

1. Enter and exit Heads (Function + triple-tap Play, both directions)
2. Function + Fader N — audible positional scrub of lane N, with grain output measured
3. Function + double-tap Track N — lane reverse (loop-only when a loop exists)
4. Track hold — momentary audition, **including while the transport is paused**
5. Two, three and four simultaneous Track holds — group audition, exact mix restored on release
6. Track double-tap — one-bar loop capture at the parked scrub position; single tap releases it
7. Track triple-tap — latched independent playback while the transport is paused
8. FX processing while in Heads — an FX press affects the heads bus, verified by RMS/spectrum change
9. Exit restoration — stem mute/solo/level/reverse state identical, bit-for-bit, to pre-entry

Any of these that fails gets a real fix. I am not pre-committing to "no DSP changes": if the
end-to-end pass shows the fault is in `headLanes`/`engine` audio wiring rather than in
arbitration, that code is changed too. What I will avoid is speculative DSP churn.

## Step 5 — Feedback comes from the accepted acknowledgement

The "what just happened" line is built from the **engine acknowledgement**, not from current UI
selection. The ack is extended to carry the resolved target: stem/lane, FX algorithm name, bank,
and action, so the sentence is a rendering of what actually executed
("MOTION · Reel Flange — momentary on stem 2, released"), never a re-read of state that may
have moved on. Same table feeds Heads events ("Head 3 latched — playing independently at 4.21 s").

## Step 6 — Remove the memory-mode concept entirely

Not "high memory by default". Delete the mode itself: the setting, the selector, the local
storage drawer, the persisted preference and every branch that reads it. One normal automatic
configuration remains.

## Step 7 — Remaining interface work

- Controls split into (i) a pure hit-zone overlay toggle and (ii) an accordion control
  reference — "how to scrub", "how to reverse a stem", "capture a loop", "heads mode",
  "FX banks" — each drawer showing the gesture diagram plus a short animated example on the
  SP-1. Guide tab gets the same content plus a short first-session walkthrough.
- Header: "created by Mounir Sakho", linked to the Instagram profile.
- Remove the "No network request…" sentence and the System-page report download.
- "support this project" button beside the unofficial/independent line, cupped hand with a
  floating coin, opening a placeholder panel (not wired to a provider yet).
- Tempo shows nothing until a stem is decoded and analysed.
- Session name is editable and persists.

## Acceptance test (the only thing that counts as "works")

Load real stems once. Then, in **Chromium and WebKit**: perform each gesture on the rendered
SP-1 with pointer events, and for each one record the expected command, the engine
acknowledgement, the pointer/position movement, measured audible output, and the UI
explanation string. A feature is reported as working only when all five agree, in both engines.
Unit suite must also stay green.
