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

Eight independently dimmable channels, verified against three independent
sources before implementation (community `stemplayer_pins.h`/wiki, a
hardware-tested community PWM implementation on this same board, and this
firmware's own pinned Tape Looper GPIO tables) — see
`firmware/stemtape/src/led_protocol.h` for the full citation.

| Index | Logical LED | GPIO | PWM instance | PWM channel |
|---:|---|---|---|---|
| 0 | Track 1 | P0.29 | PWM2 | 0 |
| 1 | Track 2 | P0.26 | PWM2 | 1 |
| 2 | Track 3 | P1.15 | PWM2 | 2 |
| 3 | Track 4 | P1.14 | PWM2 | 3 |
| 4 | Playback/side 1 | P0.01 | PWM3 | 0 |
| 5 | Playback/side 2 | P1.12 | PWM3 | 1 |
| 6 | Playback/side 3 | P0.00 | PWM3 | 2 |
| 7 | Playback/side 4 | P1.13 | PWM3 | 3 |

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

## 2. Transport: MIDI channel 16

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
| 89 | Heartbeat | most recently committed sequence |
| 90 | Release host ownership, clear the runtime frame | ignored |
| 91 | Capability query | must be `0` to trigger a response |

Device → host: CC91 on channel 16 with value `1` (protocol version 1) is the
capability response. A capability query never alters LED state and never
enters the normal SP-1 control decoder. Invalid channels and any CC on
channel 16 outside 80–91 are silently ignored — no echo, no error message.

## 3. Frame rules

- **Staging is invisible.** CC 80–87 only update the device's internal
  "staged" buffer; nothing is rendered until a commit.
- **The first commit requires all 8 indices staged.** After boot, after a
  fresh MIDI connection, after a release (explicit or timeout), or after a
  disconnect, the device rejects any commit until every one of the 8
  indices has been staged at least once. Send a complete all-8 frame first.
- **Later commits may update a subset.** Once ownership is established, the
  device remembers every previously staged value, so a later commit only
  needs to stage the indices that actually changed — the rest keep their
  last value.
- **Commit is atomic.** The complete staged frame is copied to the
  rendered/active frame as one transaction; you will never see half a
  frame.
- **Duplicate commits are idempotent.** Re-sending the same sequence number
  is a safe no-op, not an error.
- **Host ownership begins only after the first valid complete commit** —
  never from staging alone, and never from a heartbeat.

## 4. Sequence numbers (modulo-128 wraparound)

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

## 5. Heartbeat and lease

- Send CC89 (heartbeat) **at least every 250 ms** while you want to keep
  LED ownership, with its value set to your most recently committed
  sequence.
- The device's lease **times out after 1000 ms** with no commit or
  heartbeat.
- A heartbeat **never creates ownership by itself** — only a valid complete
  commit does.
- On timeout, an explicit release (CC90), or a USB MIDI disconnect, the
  device clears ownership, clears staging completeness, and turns the
  runtime frame off. **A stale frame never remains illuminated after the
  website disappears.** You must send a fresh complete 8-channel frame to
  resume control.

## 6. Brightness → duty mapping

Level `0…127` maps **linearly** to `0…rowMaximumPulseUs` for the physical
index's row, at a **1024 µs PWM period**:

- Track row (indices 0–3): ceiling **52 µs**.
- Playback row (indices 4–7): ceiling **66 µs**.

`0` is always truly off. `127` is always exactly the row's ceiling. These
ceilings are the device's existing electrical brightness limits and will
not be raised without a separately reviewed electrical justification —
sending `127` is always safe.

## 7. Safety precedence

The device's own safety behavior always outranks a host frame, in this
order:

1. Early DFU and recovery indication (Track 1 + Track 4 hold)
2. Fatal/reset and shutdown handling
3. FUNCTION hold-to-power-off countdown
4. Boot signature (two flashes at power-on)
5. A valid leased host frame (this protocol)
6. Local idle fallback (LEDs off) when no host frame is leased

A host frame can never hide the DFU cue, the shutdown countdown, or the
boot signature — the device will visibly override your frame during those
states, and resume showing it afterward once you still hold the lease.

## 8. TypeScript constants (copy into the web app)

```ts
// Stem Tape LED Feedback Protocol v1 — must mirror
// firmware/stemtape/src/led_protocol.h exactly.

export const LED_PHYSICAL_COUNT = 8;

export const LED_IDX = {
  TRACK1: 0, TRACK2: 1, TRACK3: 2, TRACK4: 3,
  PLAYBACK1: 4, PLAYBACK2: 5, PLAYBACK3: 6, PLAYBACK4: 7,
} as const;

// MIDI channel 16, zero-indexed as the wire/UMP value.
export const LED_MIDI_CHANNEL = 15;

export const LED_CC = {
  STAGE_FIRST: 80,   // 80..87: stage physical index 0..7
  STAGE_LAST: 87,
  COMMIT: 88,        // value = frame sequence 0..127
  HEARTBEAT: 89,      // value = most recently committed sequence
  RELEASE: 90,
  CAPABILITY: 91,    // send value 0 to query; device replies value 1
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

function sendHeartbeat(output: MIDIOutput, lastCommittedSeq: number) {
  output.send([0xB0 | LED_MIDI_CHANNEL, LED_CC.HEARTBEAT, lastCommittedSeq & 0x7f]);
}
```

## 9. Evidence

- Current Stem Tape M0 source (`firmware/stemtape/`) and its pinned Tape
  Looper provenance (commit `a8dd127ba1d595e54f92503a0bd75eabca86334d`).
- `timknapen/SP-1-dev` — `stemplayer_pins.h` and the Hardware-overview/PWM
  wiki pages (community pinout + "PWM drives the track LEDs... and the
  playback LEDs" confirmation).
- `bnjreece/feldd-sp1-firmware` (MIT-licensed) — `src/led.c`,
  `src/led_override.c`/`.h`, `app.overlay`: a hardware-tested PWM2/PWM3
  implementation on this same board, confirming the exact GPIO map above and
  the PWM2 (track) / PWM3 (playback) / 1024 µs period layout this firmware
  adapts. Attribution preserved in `firmware/stemtape/app.overlay` and
  `firmware/stemtape/src/led_render.c`.

No physical device has been flashed or tested as part of writing this
protocol or the firmware that implements it (see the firmware repo's build
report for what has and has not been verified). Do not describe any built
image as verified-safe-to-flash based on this document alone.
