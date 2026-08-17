# Stem Tape LED Feedback Protocol v1

Integration handoff for the website team. This document is the single
source of truth for the wire protocol; the firmware constants live in
`firmware/stemtape/src/led_protocol.h` and must never drift from this table.

**The website owns all state.** Gesture, loop, scrub, FX, mixer, mute/solo
and LED-precedence logic all live in the web app. The firmware never infers
behavior — it stages, atomically commits and renders an already-resolved
8-value brightness frame under a lease. Do not build a second state machine
against this protocol; send the final answer.

## 1. Physical LED map

Exactly **eight** MCU-controllable LEDs: four Track LEDs and four side
battery/Play LEDs. **The Function-button dots and the red triangle are
static enclosure markings, not LEDs** — there is no ninth or tenth channel.

| Index | Logical LED | GPIO | PWM instance | PWM channel | Gauge step |
|---:|---|---|---|---|---:|
| 0 | Track 1 | P0.29 | PWM2 | 0 | — |
| 1 | Track 2 | P0.26 | PWM2 | 1 | — |
| 2 | Track 3 | P1.15 | PWM2 | 2 | — |
| 3 | Track 4 | P1.14 | PWM2 | 3 | — |
| 4 | Side, nearest PLAY | P1.13 | PWM3 | 3 | 1 (bottom) |
| 5 | Side, PLAY-side middle | P0.00 | PWM3 | 2 | 2 |
| 6 | Side, FUNCTION-side middle | P1.12 | PWM3 | 1 | 3 |
| 7 | Side, nearest FUNCTION | P0.01 | PWM3 | 0 | 4 (top) |

Note the PWM channel column is **reversed** for the side row relative to
index order (index 4 → channel 3 … index 7 → channel 0) — this is real,
verified electrical wiring (`app.overlay`), not a typo. This table is the
device firmware's **one authoritative hardware table**
(`led_channel_table[]` in `firmware/stemtape/src/led_duty.h`); the renderer,
the diagnostic sweep, the CDC diagnostics, and this document all read the
same table, so a website integrator reading a `led_sweep:` CDC line will see
exactly these GPIO/PWM/gauge-step values, never a re-derived approximation.
"Gauge step" is the bottom-to-top position used by the local charging gauge
(section 2) — 1 is the first LED to light, 4 is only lit at a full/complete
charge.

**The GPIO set for the side row is confirmed** by three independent sources
(community `stemplayer_pins.h`, a hardware-tested community PWM
implementation on this same board, and this firmware's own pinned Tape
Looper GPIO arrays — see "Evidence" below). **The PLAY-end/FUNCTION-end
direction is NOT confirmed.** The two community sources number that same
4-GPIO set contradictorily — one source's "LED_1" is the other's "PLAY4" —
so a symbolic name in either source carries no information about physical
position. The direction above is this firmware's best-effort inference (its
own pinned Tape Looper GPIO array order, the only fixed reference point this
repository owns), confirmed or corrected only by physically running the
firmware's eight-step diagnostic sweep (type `s` into the CDC console) and
watching the real device. **Do not treat this direction as ground truth
until that sweep has actually been run on hardware.**

There is **no separate Function-button LED channel** — FUNCTION (P0.27) is a
plain input GPIO with no emitter of its own.

**The website's `play-indicator` visual and the two Function-dot visuals are
NOT physical LED channels.** They are host-side illustration only, drawn in
the web UI. Do not map them to a CC 80–87 index; there is no physical LED
for either.

Never drive P0.22 (BQ24232 `nCHG`), P0.24 (BQ24232 `nPGOOD`), or P0.21
outside the firmware's own charger-enable implementation — those are
charger-control/status nets, not available LED outputs, and are not part of
this protocol in any way.

## 2. Battery / charging baseline (stock local behavior)

**The side row is not four generic effect-bank LEDs and not an arbitrary
function of the compressed CC24 value — it is the SP-1's own documented
4-step charging gauge**, driven from the real BQ24232 charger status pins
(`nCHG` P0.22, `nPGOOD` P0.24, both open-drain/active-LOW) and the raw AIN4
battery-divider ADC reading, ported from the pinned Tape Looper firmware's
own standby gauge (see "Evidence"). It is always available with no browser
connected.

**Local gauge states.** The firmware classifies its own battery/charger
reading into exactly one of six states every tick
(`led_battery_classify()`):

| State | Meaning |
|---|---|
| `UNAVAILABLE` | No valid ADC sample has ever landed (e.g. just after boot). |
| `FAULT` | Either the most recent ADC read failed after a prior valid one, or the charger status pins report a combination the BQ24232 should never produce (`nCHG` asserted while `nPGOOD` is deasserted). |
| `CHARGER_ABSENT` | On battery, valid reading, above the low threshold — ordinary operation. |
| `CHARGING` | Charger present (`nPGOOD` asserted) and actively charging (`nCHG` asserted). |
| `CHARGE_COMPLETE` | Charger present, not actively charging. |
| `LOW` | On battery, valid reading, at/below the low threshold. |

**`LOW` is the ONLY state allowed to preempt an owned host frame.** An
unavailable or failed ADC reading, or a charger-status fault, is **never**
classified as low and **never** suppresses a valid, leased host frame —
including at startup, before the first ADC sample has ever landed. This is a
correction from an earlier revision, which incorrectly treated "no reading
yet" as "empty battery" and could silently override a freshly-leased host
frame during the first second after boot.

- **While a valid host lease is held and the battery state is not `LOW`:**
  the firmware renders the complete 8-value **host-committed frame
  verbatim** — including the side LED nearest PLAY — with no element-wise
  merge with the local gauge, regardless of whether the device also happens
  to be `CHARGING` or `CHARGE_COMPLETE` at that moment. If you want the
  PLAY-adjacent LED fully illuminated to indicate "playing," commit a frame
  with index 4 (and whatever you choose for the other 7) via the ordinary
  stage/commit flow in section 4. There is no separate "play" message —
  playback state is conveyed entirely by what you choose to commit.
- **`LOW` outranks a leased host frame.** This is a safety-tier behavior,
  listed in the precedence table in section 8.
- **On host release, MIDI disconnect, lease timeout, or an unrecovered
  renderer fault, the firmware reverts immediately and deterministically to
  the local gauge above — never to all-LEDs-off.**

**Local gauge rendering** (`led_battery_gauge_frame()`), matching the pinned
looper's own documented gauge exactly: 1–4 side LEDs solid **below** the
current charge level (bottom-to-top, per the "Gauge step" column in section
1); the current level's LED is **solid** when not charging, or **blinks
(~1 Hz)** while `CHARGING`; all four solid once `CHARGE_COMPLETE`. When the
gauge has never been seeded (`UNAVAILABLE` with no prior data at all), the
entire side row is left **off** — never fabricated as a specific charge
level.

**Provisional calibration.** The raw-ADC thresholds that divide the gauge
into quarters (2020 / 2140 / 2260, ±18-count hysteresis, 1/8 EMA smoothing)
are copied verbatim from the pinned Tape Looper source, which itself labels
them *"PLACEHOLDERS until calibrated" / "Interim calibration"* — see
"Evidence". M0 inherits that same provisional status unchanged; do not
present these as a final hardware calibration. The raw ADC code, smoothed
EMA, gauge level, and the two charger status pin readings are all exposed
on the CDC diagnostic stream (a `batt:` line, tagged `calib=PROVISIONAL`)
for anyone re-running a bench calibration.

In short: **the local gauge is the LED protocol's idle state**, not a
separate mechanism. The composition rule is "host frame wins outright while
leased and the battery state is not the *valid* `LOW` reading, local gauge
wins otherwise" — never a partial/blended merge, and never based on a
reading the firmware does not actually trust.

## 3. Transport: MIDI channel 16

Reserve **MIDI channel 16** (1-indexed; wire value 15) for this protocol.
Existing surface controls (buttons, faders, battery) stay on **MIDI channel
1** exactly as before — never mix the two. **The website must send LED
Feedback Protocol traffic only to the matching Stem Tape SP-1 MIDI output**
(the same device the surface controls arrive from); sending it to any other
MIDI output does nothing useful and risks driving an unrelated device.

Control Change messages on channel 16:

| CC | Meaning | Value |
|---:|---|---|
| 80–87 | Stage brightness for physical LED index (CC − 80) | 0–127 |
| 88 | Commit the staged frame | frame sequence, 0–127 |
| 89 | Heartbeat | must equal the most recently committed sequence |
| 90 | Release host ownership, clear the runtime frame | ignored |
| 91 | Capability query | must be `0` to trigger a response |

Device → host: CC91 on channel 16 with value `1` (protocol version 1) is the
capability response — **sent only if every one of the eight physical
outputs is CURRENTLY usable**, checked fresh against live renderer state,
not just a one-time boot flag. This is true right after boot only if
initialization succeeded, and can become false again later at runtime if a
channel accumulates enough consecutive write failures to fault the renderer
(section 7) — the response is suppressed the instant that happens, and
resumes only once every channel has been proven usable again (an explicit
firmware recovery). If the renderer is not currently ready, the device sends
no response at all to a capability query (never a false "supported"); this
is distinguishable from "device not present" only by the absence of any
other MIDI traffic from it either. A capability query never alters LED
state and never enters the normal SP-1 control decoder. Invalid channels and
any CC on channel 16 outside 80–91 are silently ignored — no echo, no error
message.

## 4. Frame rules

- **Staging is invisible.** CC 80–87 only update the device's internal
  "staged" buffer; nothing is rendered until a commit.
- **The first commit requires all 8 indices staged.** After boot, after a
  fresh MIDI connection, after a release (explicit, timeout, disconnect, or
  a renderer failure), the device rejects any commit until every one of the
  8 indices has been staged at least once since that reset. Send a complete
  all-8 frame first. A release also clears any values staged *before* it —
  they can never silently complete a future partial frame.
- **Later commits may update a subset.** Once ownership is established, the
  device remembers every previously staged value, so a later commit only
  needs to stage the indices that actually changed — the rest keep their
  last value.
- **Commit is atomic in firmware state.** The complete staged frame is
  copied to the rendered/active frame as one transaction; you will never
  read back half a frame. **This is not a claim about physical
  simultaneity** — see section 7.
- **Duplicate commits are idempotent.** Re-sending the same sequence number
  is a safe no-op, not an error.
- **Host ownership begins only after the first valid complete commit** —
  never from staging alone, and never from a heartbeat.

## 5. Sequence numbers (modulo-128 wraparound)

The commit sequence is a 7-bit value (0–127) that must **strictly
increase** with each new commit, wrapping from 127 back to 0. The device
compares sequences using the standard half-window rule:

```
diff = (new_seq - last_committed_seq) mod 128
diff == 0        -> duplicate (idempotent, not an error)
1 <= diff <= 63   -> newer: accepted
64 <= diff <= 127 -> stale/out-of-order: rejected, no state change
```

Simply increment a counter (mod 128) on every commit; the wraparound is
handled correctly by the rule above. Do not skip more than 63 sequence
numbers between two commits the device is expected to accept.

## 6. Heartbeat and lease

- Send CC89 (heartbeat) **at least every 250 ms** while you want to keep
  LED ownership, with its value set to your **most recently committed**
  sequence number, exactly.
- **The heartbeat value must equal the last committed sequence, exactly —
  not "close," not "ahead," not "behind."** A mismatched heartbeat (stale
  OR sent early with a sequence you have not actually committed yet) is
  rejected and does **not** extend the lease. If you commit and then
  heartbeat, always heartbeat with the sequence from your most recent
  *commit*, not an internal counter that may have moved on.
- The device's lease **times out after 1000 ms with no commit or matching
  heartbeat.** Elapsed time is computed with wraparound-safe arithmetic on
  the device's internal millisecond clock — nothing you need to do
  differently, this only affects the device's own long-uptime correctness.
- A heartbeat **never creates ownership by itself** — only a valid complete
  commit does.
- On timeout, an explicit release (CC90), or a USB MIDI disconnect, the
  device clears ownership, all staged values, and the runtime frame, and
  reverts immediately to the local battery/Play baseline (section 2) — never
  to all-LEDs-off and never leaving a stale frame lit. You must send a fresh
  complete 8-channel frame to resume control.

## 7. Brightness → duty mapping, and physical update timing

Level `0…127` maps **linearly** to `0…rowMaximumPulseUs` for the physical
index's row, at a **1024 µs PWM period**:

- Track row (indices 0–3): ceiling **52 µs**.
- Side row (indices 4–7): ceiling **66 µs**.

`0` is always truly off. `127` is always exactly the row's ceiling. These
ceilings are the device's existing electrical brightness limits and will
not be raised without a separately reviewed electrical justification —
sending `127` is always safe.

**Physical update timing is NOT guaranteed simultaneous across channels.**
A commit is atomic only in firmware state (section 4): the device renders it
as up to eight independent hardware PWM register writes, one per changed
channel, skipping channels whose value did not change since the last
render. Per Zephyr's nRF PWM driver
([`pwm_nrfx.c`](https://github.com/zephyrproject-rtos/zephyr/blob/main/drivers/pwm/pwm_nrfx.c)),
each of those writes can restart the shared hardware sequence for every
channel on the same PWM instance — PWM2 serves all four Track channels
together, PWM3 serves all four side channels together — so multiple
channels on the same instance that change together in one commit are not
guaranteed to visually transition at the same instant; up to one PWM period
(1024 µs) of stagger between them is physically possible. **This has not
been measured on real hardware.** If your use case depends on
sub-millisecond synchronized transitions across all four Track (or all four
side) LEDs, do not assume it — request a hardware timing measurement first.

**Write reliability, retry, and fault handling.** Each of the (up to eight)
physical writes a render performs can itself fail at the driver level. The
firmware never presents a failed write as if it had succeeded:

- A channel's cached "last known good" level is updated **only when its
  write actually succeeds.** A failed write leaves that channel's cache
  invalidated and marks it dirty, so the exact same requested level is
  retried, deterministically, on the very next render (every 5 ms).
- If one channel fails **3 consecutive** writes, the renderer as a whole
  latches **not-ready**: the CC91 capability response is suppressed
  (section 3), the device forces a best-effort safe (all-off) write across
  every channel, and — if a host frame was leased at the time — that lease
  is released exactly like an explicit release or a timeout (section 6),
  reverting the side row to the local gauge (section 2).
- Recovery is an explicit re-initialization that re-proves all eight
  channels from scratch; the renderer only reports ready again once every
  one of the eight succeeds. There is no implicit "it'll probably come back
  on its own" behavior.
- A steady, fully healthy frame costs **zero** PWM writes per render call —
  only channels that changed, or that are still dirty from a prior failure,
  are ever written.

## 8. Safety precedence

The device's own safety behavior always outranks a host frame, in this
order:

1. Early DFU and recovery indication (Track 1 + Track 4 hold) — held for
   300 ms, a genuinely human-visible duration by common flash-legibility
   convention (not merely the ~1 ms electrical minimum needed for the PWM
   hardware to reach the commanded duty); real-user visibility has not been
   confirmed on hardware.
2. Fatal error handling — reboots immediately with **no LED indication at
   all**; the next LED event is the following boot's boot signature.
3. Shutdown (FUNCTION hold-to-power-off) countdown and handling.
4. Boot signature (two flashes at power-on).
5. `LOW` battery (section 2) — outranks a leased host frame, not the states
   above it. Only the valid `LOW` state; `UNAVAILABLE`, `FAULT`,
   `CHARGER_ABSENT`, `CHARGING` and `CHARGE_COMPLETE` never preempt a host
   frame.
6. A valid leased host frame (this protocol) — released automatically, and
   demoted to the next tier below, if the renderer latches an unrecovered
   write fault (section 7).
7. Local charging gauge (section 2) — the idle fallback whenever no host
   frame is leased (or the renderer just released one due to a fault), not
   all-off.

A host frame can never hide the DFU cue, the shutdown countdown, the boot
signature, or a valid low-battery indication — the device will visibly
override your frame during those states, and resume showing it afterward
once you still hold the lease and the battery state is no longer `LOW`.

## 9. TypeScript constants (copy into the web app)

```ts
// Stem Tape LED Feedback Protocol v1 — must mirror
// firmware/stemtape/src/led_protocol.h exactly.

export const LED_PHYSICAL_COUNT = 8;

export const LED_IDX = {
  TRACK1: 0, TRACK2: 1, TRACK3: 2, TRACK4: 3,
  // Side row, PLAY-end to FUNCTION-end — best-effort inference, confirm
  // with the firmware's diagnostic sweep before relying on this direction.
  SIDE_PLAY: 4, SIDE_MID1: 5, SIDE_MID2: 6, SIDE_FUNCTION: 7,
} as const;

// MIDI channel 16, zero-indexed as the wire/UMP value.
export const LED_MIDI_CHANNEL = 15;

export const LED_CC = {
  STAGE_FIRST: 80,   // 80..87: stage physical index 0..7
  STAGE_LAST: 87,
  COMMIT: 88,        // value = frame sequence 0..127
  HEARTBEAT: 89,     // value = your most recently COMMITTED sequence, exactly
  RELEASE: 90,
  CAPABILITY: 91,    // send value 0 to query; device replies value 1 if ready
} as const;

export const LED_PROTOCOL_VERSION = 1;
export const LED_SEQ_MODULUS = 128;
export const LED_HEARTBEAT_INTERVAL_MS = 250;
export const LED_LEASE_TIMEOUT_MS = 1000;

// Example: send a complete initial frame (all 8, in order), then commit.
function sendInitialFrame(output: MIDIOutput, levels: number[] /* [8] */, seq: number) {
  for (let i = 0; i < LED_PHYSICAL_COUNT; i++) {
    output.send([0xB0 | LED_MIDI_CHANNEL, LED_CC.STAGE_FIRST + i, levels[i] & 0x7f]);
  }
  output.send([0xB0 | LED_MIDI_CHANNEL, LED_CC.COMMIT, seq & 0x7f]);
}

// Heartbeat MUST carry the sequence from your most recent commit, exactly —
// a mismatched value (stale or ahead) is rejected and does not extend the
// lease.
function sendHeartbeat(output: MIDIOutput, lastCommittedSeq: number) {
  output.send([0xB0 | LED_MIDI_CHANNEL, LED_CC.HEARTBEAT, lastCommittedSeq & 0x7f]);
}
```

## 10. Evidence

- Current Stem Tape M0 source (`firmware/stemtape/`) and its pinned Tape
  Looper provenance (commit `a8dd127ba1d595e54f92503a0bd75eabca86334d`).
- `timknapen/SP-1-dev` — `stemplayer_pins.h` and the Hardware-overview/PWM
  wiki pages (community pinout + "PWM drives the track LEDs... and the
  playback LEDs" confirmation). Numbers the side-row GPIOs LED_1..4 in an
  order that contradicts feldd's own numbering (see below) — used only to
  confirm the GPIO *set*, never for physical-position ordering.
- `bnjreece/feldd-sp1-firmware` (MIT-licensed) — `src/led.c`,
  `src/led_override.c`/`.h`, `app.overlay`: a hardware-tested PWM2/PWM3
  implementation on this same board, confirming the GPIO set and the PWM2
  (Track) / PWM3 (side) / 1024 µs period layout this firmware adapts.
  Numbers the same GPIOs PLAY1..4 in the exact reverse order of Tim
  Knapen's LED_1..4 — this contradiction is why this firmware does not
  infer physical position from either source's symbolic names. Attribution
  preserved in `firmware/stemtape/app.overlay` and
  `firmware/stemtape/src/led_render.c`.
- `zephyrproject-rtos/zephyr`'s nRF PWM driver
  ([`drivers/pwm/pwm_nrfx.c`](https://github.com/zephyrproject-rtos/zephyr/blob/main/drivers/pwm/pwm_nrfx.c))
  — basis for section 7's physical-simultaneity caveat.
- This repository's own pinned Tape Looper source
  (`firmware/src/main.c`, commit `a8dd127`): `charger_init()` /
  `usb_present()` / `charging()` (lines 4260–4290, charger GPIO wiring and
  active levels) and the standby battery gauge (lines 4591–4647, the
  quarter-level/blink/solid gauge logic and the provisional `batt_thr`
  calibration constants section 2's local charging gauge ports verbatim).

No physical device has been flashed or tested as part of writing this
protocol or the firmware that implements it. The side row's PLAY-end/
FUNCTION-end direction, the physical-simultaneity caveat in section 7, and
the provisional battery-gauge calibration constants in section 2 are all
explicitly UNCONFIRMED on real SP-1 hardware — see the firmware repo's build
report for exactly what has and has not been verified. Do not describe any
built image as verified-safe-to-flash based on this document alone.
