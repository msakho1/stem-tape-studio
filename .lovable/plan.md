# LED Behaviour Audit — read-only

No code changed. `git status --porcelain` → empty. `git diff --stat` → empty.

## 0. Inventory (as implemented)

`LedId` (`src/machine/surface.ts:50-61`) is **11** LEDs, not 8:
`track-led-1..4`, `side-led-1..4`, `play-indicator`, `function-led-1`, `function-led-2`.
All 11 are rendered in `src/device/DeviceSurface.tsx` (tracks L177-185, side L209-217, play-indicator L204-207, function L219-227).

Single producer: `deriveLeds(state, now)` — `src/machine/surface.ts:1471-1604`.
Single consumer: `useDeviceSurface.ts:865` `useMemo(() => deriveLeds(state), [state])` → `index.tsx:303` → `DeviceSurface`.
Patterns are class names only: `ledClass()` → `st-led--<pattern>`; all visual values live in `src/styles.css:229-345`.

Pattern → CSS (core opacity / period):

| Pattern | Core | Halo | Period | Source |
| --- | --- | --- | --- | --- |
| dark | 0.08 | 0 | — | `styles.css:240` |
| faint | 0.32 | 0.06 | — | `:244,248` |
| solid | 1.0 | 0.34 | — | `:252,256` |
| pulse | 0.34→1 at 12% | 0.04→0.36 | 1.2 s ease-in-out infinite | `:260,264`, `@keyframes st-pulse :303` |
| blink | 1 / 0.12 square | — | 0.4 s steps(1,end) infinite | `:268`, `st-blink :322` |
| breathe | 0.22↔0.95 | — | 2.4 s ease-in-out infinite | `:272`, `st-breathe :331` |
| chase | 0.2→1 at 40%→0.2 | pulse-halo | 0.55 s linear infinite | `:277,281`, `st-chase :285` |

Global brightness: `data-lights="dim"` overrides core 0.28 / halo 0.05 (`:298`), overriding all patterns via specificity — **dim silently kills every animation's amplitude for non-animated patterns only** (animations still win over the static rule because animated properties beat non-animated declarations). `prefers-reduced-motion` forces `animation:none` and 0.8 opacity for pulse/blink/breathe, but **not for chase** (`:347-356`) — chase becomes frozen at core 0.1.
Phase: all animations start at element/class mount; there is **no phase anchor**, so LEDs are mutually unsynchronised and re-derive does not restart them.

## 1. State matrix (exact, as coded)

Track LEDs, evaluated in strict `if/else` order (`surface.ts:1489-1541`) — this order **is** the precedence:

| # | Condition | Track 1-4 pattern | Priority field | Source |
| --- | --- | --- | --- | --- |
| 1 | `power==="off"` | dark | 100 | :1492 |
| 2 | `grid.rejected` | blink 0.4 s | 98 | :1494 |
| 3 | heads-entry rejected, `now-headsRejectFlashAt < 420 ms` | blink | 84 | :1496 |
| 4 | FX latch flash, `now-fxFlashAt < 220 ms` | blink | 83 | :1503 |
| 5 | `perf.fxOverlay` + soloed | solid | 74 | :1509 |
| 6 | overlay + `!linked` | blink | 70 | :1511 |
| 7 | overlay + `activeStem===i` | breathe | 66 | :1513 |
| 8 | overlay + another stem soloed | faint | 10 | :1515 |
| 9 | overlay idle | faint | 10 | :1517 |
| 10 | that track button physically held | solid | 95 | :1519 |
| 11 | heads + `headLatched` | solid | 77 | :1527 |
| 12 | heads + `headMuted` | faint | 74 | :1528 |
| 13 | heads + loaded | chase 0.55 s | 76 | :1529 |
| 14 | heads + empty | faint | 75 | :1530 |
| 15 | `content==="empty"` | dark | 0 | :1532 |
| 16 | `content==="muted"` | faint | 30 | :1534 |
| 17 | `playing` | pulse 1.2 s | 20 | :1536 |
| 18 | loaded + stopped | faint | 10 | :1538 |

Side LEDs 1-4 (`:1544-1568`):

| Condition | Pattern | Source |
| --- | --- | --- |
| power off | dark | :1546 |
| overlay + algorithm `rejected` | blink | :1557 |
| overlay + `bank.momentary` | breathe | :1559 |
| overlay + `bank.latched` | solid | :1561 |
| overlay, bank inactive | dark | :1562 |
| `bankJumpArmed && i===bank` | blink | :1564 |
| `i === song % 4` | solid | :1566 |
| else | dark | :1567 |

`play-indicator` (`:1573`): off→dark; `playing`→solid; else faint. Red core, `st-led--signal`.
`function-led-1` (`:1577`): off→dark; overlay→pulse; `functionHeld`→solid; `headsMode`→breathe; else dark.
`function-led-2` (`:1587`): off→dark; overlay→blink; `scrubLatched`→chase; any rocker pressed→blink; `grid.bpm != null`→pulse; else dark.

## 2. Requested states → what actually exists

| Requested state | Implemented? | Evidence |
| --- | --- | --- |
| idle / loaded | Yes (rows 15,18) | :1532,:1538 |
| playing | Yes — track pulse + play-indicator solid | :1536,:1573 |
| stopped | Yes | :1538,:1575 |
| **paused** | **Missing** — no paused state exists; only boolean `state.playing` | — |
| active stem | Partial — breathe, **only while FX overlay is open** | :1513 |
| mute | Partial — `content==="muted"` faint outside overlay; per-stem `perf` mute not shown | :1534 |
| solo | Partial — solid, **overlay only** | :1509 |
| momentary global loop | **Missing** — `globalLoop.active` never read by `deriveLeds` | grep: `globalLoop` absent from :1471-1604 |
| latched global loop | **Missing** — `globalLoop.latched` never read | same |
| loop-division selection | **Missing** — `globalLoop.division` never read | same |
| momentary fwd/rev scrub | Partial — function-led-2 blink for *any* rocker press; no direction | :1592 |
| latched fwd/rev scrub | Partial — chase; direction only in the `reason` string, **same pattern both ways** | :1594-1600 |
| four scrub-speed levels | **Missing visually** — `scrubSpeed` appears only inside `reason` text | :1597 |
| scrub inertia handoff | **Missing** | no reference |
| STEM vs GLOBAL FX scope | **Missing** — `fxTargetOf` only picks *which* rack to display; no LED distinguishes scope | :1553 |
| FX-bank selection | Partial — the four side LEDs *are* the banks; the **selected algorithm within a bank is not shown** | :1554-1556 |
| momentary FX | Yes — breathe | :1559 |
| latched FX | Yes — solid | :1561 |
| FX latch/unlatch confirm flash | Partial — 220 ms window, but rendered as infinite `blink`, and see §3 defect | :1481,:1503 |
| clear-all latches | **Missing** — `fx.clearLatches` sets no flash | :1425-1432 |
| Heads mode | Yes — chase/solid/faint per head + function-led-1 breathe | :1521-1531,:1583 |
| heads rejected | Yes — 420 ms blink (described as "double flash", is not) | :1496 |
| grid rejected | Yes — all four blink | :1494 |
| tempo grid present | Yes — function-led-2 pulse (fixed 1.2 s, **not BPM-locked**) | :1594 |
| loading / decode in progress | **Missing** | — |
| errors (engine, decode, export) | **Missing** — only `grid.rejected` and FX `alg.rejected` | — |
| MIDI connect/disconnect | **Missing** — no LED path reads MIDI state | grep: no midi ref in `deriveLeds` |
| recording / overdub / print | **Priority constants exist, no branch uses them** (`recording:90`, `overdubbing:89`, `armed:88`, `printing:93`, `failedPrint:95`) | :1447-1463 |

## 3. Defects found

1. **Flash windows never expire.** `fxFlashAt` / `headsRejectFlashAt` are compared against `now` computed inside `deriveLeds`, but the memo key is `[state]` (`useDeviceSurface.ts:865`) and no timer schedules a re-render at +220/+420 ms. The flash therefore persists until the next unrelated dispatch, and if the next dispatch is >window it disappears instantly with no minimum on-time. Neither duration is honoured on screen.
2. **One-shot flashes are rendered as infinite animations.** `blink` is `infinite`; there is no one-shot keyframe anywhere. "Two rapid flashes" (heads reject) and "four-light flash" (FX latch) are visually identical to `grid.rejected` and to `!linked` stems.
3. **`priority` is decorative.** Every branch writes a `priority` number but arbitration is the `if/else` order, which contradicts the stated table: e.g. button-held (95) is evaluated *after* the overlay branches (10-74), and heads latched (77) after overlay solo (74). `LED_PRIORITY` is never read to choose a winner.
4. **`pulse` is not loop-locked.** The reason string says "pulses on its own loop wrap"; the CSS is a fixed 1.2 s cycle with no per-track phase — so the documented v2.6 polyrhythm is not produced.
5. **`chase` ignores direction and rate.** `headReverse` changes only the reason text; forward and reversed chase animate identically at 0.55 s.
6. **`prefers-reduced-motion` does not cover `chase`** → frozen at inherited 0.1 opacity.
7. **`data-lights="dim"` is inconsistent** — it dims static patterns but cannot dim animated ones.

## 4. Conflicting states with no explicit precedence

- Track button held vs FX overlay open: overlay wins by ordering, contradicting priority 95 > 74.
- Heads mode + FX overlay: overlay wins; heads head-state is invisible while the overlay is open.
- `grid.rejected` is sticky-ish (state flag, not timed) and outranks every flash and the overlay.
- Latched shuttle vs rocker held vs grid BPM all compete for `function-led-2` with no combined encoding — a latched shuttle hides the grid indication permanently.
- FX overlay hides the song row (side LEDs) with no fallback.

## 5. Is there a reusable resolved frame for physical MIDI output?

Partially. `deriveLeds(state, now)` is already a pure `SurfaceState → LedFrame` function with stable ids and a closed pattern vocabulary, so it is the right seam. It is **not yet consumable by hardware** because:

- it is only invoked from a React `useMemo` keyed on `state`, so time-dependent frames (flashes, and anything phase-locked) are never re-evaluated;
- `LedFrame` carries a pattern name, not brightness/period/phase — those live in CSS, so a device would have to re-derive the timing table independently and would drift;
- no one-shot / duration semantics exist in the type (`LedState` = pattern + reason + priority);
- `priority` is present but unused, so a second consumer would resolve conflicts differently from the DOM.

### Smallest architecture to make web + device share one authoritative model

1. Extend `LedState` to `{ pattern, periodMs, phaseAnchorMs, brightness, durationMs | null, priority, reason }` — timing moves out of CSS into the frame.
2. Make arbitration data-driven: each branch pushes a candidate; a single reducer picks max `priority`. Deletes the if/else-vs-priority contradiction.
3. Add a clock: a single rAF/interval ticker in `useDeviceSurface` re-derives whenever any frame carries a live `durationMs` or phase anchor, so flashes expire deterministically.
4. Add one-shot patterns (`flash1`, `flash2`) driven by `durationMs`, and drive CSS via inline `animation-duration` / `animation-delay` from the frame instead of fixed class periods.
5. Expose `deriveLeds` through a transport-agnostic `LedSink` interface; the DOM renderer and a MIDI SysEx/CC emitter both subscribe to the same emitted frame.

Steps 1-3 are the minimum for correctness; 4-5 are what physical parity requires.

## 6. Verification

- `git status --porcelain` → empty
- `git diff --stat` → empty
