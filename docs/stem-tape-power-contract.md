# The Stem Tape power contract

**Status: NOT COMPLETE.** Everything below is host-proven, mutation-proven,
ARM-build-proven or structurally inspected. **Nothing here has been exercised on
an SP-1.** §5 is the acceptance procedure that closes it and §6 is the honest
proof matrix. Do not read a green CI run as "safe" — that exact inference
carried a 24.7-second defect through three commits.

---

## 1. The contract

```
POWER ON    OFF → FUNCTION only, continuously, 2.000 s → ON
POWER OFF   ON  → FUNCTION only, continuously, 5.000 s → OFF
```

**A power hold is a self-contained physical transaction. Nothing that happened
before FUNCTION went down may influence it.**

On the **rising edge** of FUNCTION:

* elapsed → 0
* the settled "other control active" verdict and its candidate → cleared
* every fader baseline → dropped, so the next sample of each re-seeds
* no stale feature, ladder, fader, combo, gesture, transport or dispatcher
  state carries in

**During** the transaction, exactly three things can happen:

| | |
|---|---|
| FUNCTION released | elapsed = 0 |
| intentional other physical activity | elapsed = 0 |
| otherwise | elapsed advances monotonically |

Nothing outside the transaction may extend, suppress, pause, restart or poison
it.

### The two invariants, which pull against each other

**Safety.** *I am never more than one clean 5.000-second FUNCTION-only hold away
from power-off* — from every reachable firmware state.

**Performance.** *No legitimate FUNCTION chord may accidentally power the device
off while another control remains intentionally active.*

They are reconciled by **reset, never suppress**: another control resets the
timer to zero rather than latching it shut, so a chord can be held indefinitely
and the escape hatch is never more than 5.000 s away from the moment everything
but FUNCTION is released.

### The thresholds

| | Constant | Value | Passes @ 8 ms |
|---|---|---|---|
| ON | `ST_PWR_ON_MS` | **2000 ms** | 250 |
| OFF | `ST_PWR_OFF_MS` | **5000 ms** | 625 |
| input settle | `ST_PWR_SETTLE_PASSES` | 2 passes (~16 ms) | 2 |
| fader movement | `ST_PWR_FADER_MOVE_COUNTS` | **16 counts — UNMEASURED, see §4** | — |
| fader activity hold | `ST_PWR_FADER_ACTIVE_MS` | 250 ms | — |

`st_pwr_service()` returns **elapsed**, and both thresholds plus the LED
countdown read that one value. There is no second clock.

---

## 2. The physical control map

The rule is semantic, not convenient:

> **Any intentional physical control interaction other than FUNCTION resets the
> power transaction. Noise alone must not.**

An earlier version took "settled AIN0 only" and excluded AIN1 because that rail
is noisy. That defined a safety rule by what was easy to sample, and
FUNCTION + rocker is a real performance interaction — a timer blind to the
rocker would shut the device down underneath one.

| Control | Rail | Decode | Settling / debounce | Intentional vs noise | Resets the timer? |
|---|---|---|---|---|---|
| **FUNCTION** | `PWR_PORT` P-pin, dedicated GPIO | one register read, active-low | none needed — mechanical switch on its own pin, not on BTN_COM | n/a — it *is* the subject | n/a |
| **PLAY** | AIN0 ladder, band ~1790–4095 (centre 1813) | `st_ladder_classify()` against `docs/ladder-measured.json` | `ST_LADDER_SETTLE_READS` = 3 agreeing reads (~24 ms); ±6 hysteresis on the settled row; guard zones between bands are UNKNOWN and hold the last state | a measured band held for 3 passes; guard-zone readings never build a candidate | **yes** |
| **Track 1** | AIN0, 180–230 | same | same | same | **yes** |
| **Track 2** | AIN0, 375–425 | same | same | same | **yes** |
| **Track 3** | AIN0, 702–752 | same | same | same | **yes** |
| **Track 4** | AIN0, 1188–1238 | same | same | same | **yes** |
| **Track chords** | AIN0, 11 further measured bands (T1+T2 545–595 … all four 1732–1778) | same | same | same | **yes** — any non-idle mask |
| **VOL +** | AIN1 ladder | `st_vol_decode()` band lookup | raw per pass, then `ST_PWR_SETTLE_PASSES` = 2 agreeing passes inside `st_pwr_hold_tick()` | a measured band held for 2 passes | **yes** |
| **VOL −** | AIN1 | same | same | same | **yes** |
| **rocker FWD** | AIN1, `VOL_TEMPO_UP` | same | same | same | **yes** |
| **rocker RWD** | AIN1, `VOL_TEMPO_DOWN` | same | same | same | **yes** |
| **FX entry chord** | AIN1, measured 2019–2029 plateau (`ST_VOL_CHORD_RAW`) | `st_vol_is_chord()` / decode | same | same | **yes** |
| **Fader 1** | AIN3, continuous 0–~3700 | round-robin, one per 4 control passes (~32 ms) | movement = \|Δ\| between **consecutive samples of that fader** ≥ `ST_PWR_FADER_MOVE_COUNTS`; latched active for `ST_PWR_FADER_ACTIVE_MS` | a hand produces repeated above-threshold deltas; jitter does not. **Baseline re-seeded on every FUNCTION rising edge, so nothing the fader did before the hold can affect it** | **yes** |
| **Fader 2** | AIN6 | same | same | same | **yes** |
| **Fader 3** | AIN2 | same | same | same | **yes** |
| **Fader 4** | AIN7 | same | same | same | **yes** |
| battery divider | AIN4 | — | — | not a control | **no** |

**Faders are not sampled at all outside a transaction**, and are not sampled in
charge-standby (`fader_raw = -1`, every fader unseeded). Inside a transaction
exactly one fader is read per pass, replacing the ordinary round-robin read that
the FUNCTION branch's `continue` already suppresses — so the pass performs
**three `ladder_read()` calls whether FUNCTION is held or not**, the same count
the measured-good baseline performs during ordinary play and a third of what
st55 did.

### Why the input settle is symmetric

Both failure directions are real and pull opposite ways:

* **too eager to call a control active** → noise resets the timer forever,
  shutdown unreachable. This is the st55 class of bug through a different door.
* **too slow** → the device powers off in the middle of a gesture.

So neither edge is taken on one sample; 2 agreeing passes commit a change in
either direction.

---

## 3. Where the decision lives

```
main.c  power_hold_service()          I/O ONLY
        ├─ pwr_pressed()              1 GPIO read
        ├─ st_ladder_mask/play()      already-settled, this pass's own reading
        ├─ st_vol_decode()            this pass's own reading
        ├─ ladder_read(one fader)     only while FUNCTION is down
        ├─ k_uptime_get()
        └─ st_pwr_service(&s_pwr, &in, &out)     ← EVERY DECISION, PURE
             └─ if (out.off_due) power_off();    ← the only platform action
```

`st_pwr_in_t` has **no field a feature could set**; `power_hold_service()` has
no branch that could gate the call and no dispatcher state in scope. Both are
asserted by the wiring gate, and the pure half is driven end to end by
`tests/test_power_hold.c`.

The glue is this small because **the last version was not, and the part that was
not pure was the part that was wrong.**

---

## 4. M3 — the one unmeasured number, and how to measure it

`ST_PWR_FADER_MOVE_COUNTS = 16` (of ~3700 counts travel, sampled once per
~32 ms ≈ 500 counts/s) is **an estimate of the noise floor, not a measurement.**
AIN0 and AIN1 both have measured band tables behind them
(`firmware/stemtape_player/docs/ladder-measured.json`, `ain1-measured.json`);
the fader rails have no equivalent.

### Captures required before this number is settled

| # | Condition | Why it matters |
|---|---|---|
| M3.1 | resting fader, device idle | the floor |
| M3.2 | resting fader, playback running | I²S + mixer activity on the rails |
| M3.3 | resting fader, eMMC streaming | the streamer is the loudest aggressor |
| M3.4 | resting fader, USB active | st55's phantom-solo mechanism |
| M3.5 | resting fader, FX / pitch / reverse engaged | worst-case CPU and switching |
| M3.6 | very slow intentional movement | the low end of what must be detected |
| M3.7 | ordinary performance movement | the normal case |
| M3.8 | fast movement | the high end |

Choose the threshold from the **measured separation** between max(M3.1–M3.5) and
min(M3.6), with the margin written down.

### The failure profile, bounded

The delta-plus-edge-reset design bounds both directions, which the previous
displacement design did not:

* **over-sensitive** → delays a shutdown by `ST_PWR_FADER_ACTIVE_MS` (250 ms)
  per noise event, and could only *block* one if noise exceeded the threshold
  continuously for the full 5 s.
* **under-sensitive** → misses a movement slower than the threshold, costing
  protection during that move but never costing the escape hatch.

Neither can strand seconds of delay. **The displacement version could, and did:
19.7 s.**

---

## 5. Hardware acceptance — required before Stage 0 closes

Nothing in this section has been run. Record the **measured** time for each,
not the compiled constant.

### 5.1 Power-on — from charge-standby, then again from true battery/off wake

| # | Action | Required | Measured |
|---|---|---|---|
| ON-1 | tap FUNCTION | stays off | |
| ON-2 | hold 100 ms | stays off | |
| ON-3 | hold 600 ms | stays off *(was the old threshold — must now fail)* | |
| ON-4 | hold 1.999 s | stays off | |
| ON-5 | hold 2.000 s cleanly | turns on | |
| ON-6 | release at ~1.5 s, re-press | full fresh 2.000 s needed | |
| ON-7 | at ~1.9 s press a Track | timer resets | |
| ON-8 | release that Track, keep holding | full fresh 2.000 s needed | |
| ON-9 | 20 short taps in a row | never turns on | |
| ON-10 | ON-1…ON-9 repeated from **battery/off wake** | identical behaviour to standby | |
| ON-11 | turn on, then **keep holding FUNCTION** without releasing | see below | |

**ON-11 is an open behavioural question, not a known-good expectation.** The
standby loop calls `st_pwr_reset()` before it breaks, so the transaction that
turned the device on is closed and a finger still on FUNCTION starts a *fresh*
one in the main loop. Read literally — "from ON, FUNCTION only for 5.000
continuous seconds is OFF" — that is correct: ~7 s of unbroken hold turns the
device on and then off again. In practice the boot sequence (eMMC bring-up,
library load) consumes part of that window and the player has released long
before, so this may never be reachable. **Which of the two happens cannot be
determined from source.** Measure it, and if it is reachable, the owner decides
whether the ON transition should additionally suppress the OFF timer until
FUNCTION is next released. Do not "fix" it before it is measured.

### 5.2 Power-off

| # | Action | Required | Measured |
|---|---|---|---|
| OFF-1 | FUNCTION only, 4.999 s | stays on | |
| OFF-2 | FUNCTION only, 5.000 s | powers off | |
| OFF-3 | release at ~3 s, re-press | full fresh 5.000 s | |
| OFF-4 | at ~4.9 s press PLAY | timer resets | |
| OFF-5 | at ~4.9 s press each Track | timer resets | |
| OFF-6 | at ~4.9 s move the rocker (both ways) | timer resets | |
| OFF-7 | at ~4.9 s press VOL ± | timer resets | |
| OFF-8 | move **each** of the four faders during the hold | timer resets | |
| OFF-9 | after each of OFF-4…OFF-8, stop and keep holding | full fresh 5.000 s | |
| OFF-10 | move a fader **before** pressing FUNCTION, then hold | **exactly 5.000 s** — this is the 24.7 s defect | |

### 5.3 Performance safety — hold each chord > 5 s, device must NOT power off

FUNCTION + PLAY · FUNCTION + each Track · FUNCTION + Track double-tap (reverse)
· FUNCTION + rocker FWD · + rocker RWD · + VOL ± · + the FX entry chord ·
bank jump (FUNCTION + Track) · grid clear (tap run then hold) · slow-mode
toggle (FUNCTION + PLAY ≥ 350 ms) · loop latch.

Then **release the second control while still holding FUNCTION** and confirm a
fresh 5.000 s countdown begins and completes.

### 5.4 Feature-state independence — from each state, a clean 5 s hold powers off

normal playback · loop active · reverse latched · pitch/semitone offset · slow
mode · FX engaged · mute/solo held · bank/grid state · transport stopped.

### 5.5 Recovery independence

| # | Action | Required |
|---|---|---|
| R-1 | with the SP-1 **off**, hold Track 1 + Track 4, insert USB | ONE track light — UF2 bootloader |
| R-2 | repeat R-1 with a deliberately broken application image | still reaches the bootloader |

---

## 6. Proof matrix — what is actually proven, and how

| Requirement | Host | Mutation | ARM build | Structural gate | HW measured | HW behaviour |
|---|---|---|---|---|---|---|
| 5.000 s exact threshold | ✅ 1 ms rig | — | ✅ | — | ❌ | ❌ |
| 2.000 s exact threshold | ✅ | — | ✅ | — | ❌ | ❌ |
| release resets at any point | ✅ 7 probes | — | ✅ | — | ❌ | ❌ |
| other control resets completely | ✅ | — | ✅ | — | ❌ | ❌ |
| fresh full hold required after | ✅ | — | ✅ | — | ❌ | ❌ |
| 11 chords held 10 s never fire | ✅ | — | ✅ | — | ❌ | ❌ |
| 12 feature states still power off | ✅ | — | ✅ | — | ❌ | ❌ |
| no feature input can reach the decision | ✅ (signature) | — | ✅ | ✅ E-3 | n/a | n/a |
| rising edge forgets everything | ✅ F9 | — | ✅ | — | ❌ | ❌ |
| stale fader cannot delay a hold | ✅ F1 | ✅ | ✅ | — | ❌ | ❌ |
| single-sample phantom cannot reset | ✅ F6 | — | ✅ | — | ❌ | ❌ |
| sub-threshold noise never blocks | ✅ F7 | — | ✅ | — | **❌ M3** | ❌ |
| real fader movement does block | ✅ F8 | — | ✅ | — | **❌ M3** | ❌ |
| ADC failure is not activity | ✅ F11 | — | ✅ | — | ❌ | ❌ |
| `power_off()` absent from gated branch | — | — | ✅ | ✅ E-1 | n/a | n/a |
| service above every dispatcher | — | — | ✅ | ✅ E-2 | n/a | n/a |
| **ADC/ladder decode on real rails** | — | — | — | — | **❌** | **❌** |
| **fader noise floor (M3)** | — | — | — | — | **❌** | **❌** |
| **actual wake timing** | — | — | — | — | **❌** | **❌** |
| **actual shutdown timing** | — | — | — | — | **❌** | **❌** |
| **the real `power_off()` path** | — | — | — | — | **❌** | **❌** |
| **battery-wake vs standby parity** | — | — | ✅ (same code) | — | **❌** | **❌** |
| **bootloader independence** | — | — | — | — | **❌** | **❌** |

### Manually inspected only — no mechanical proof

* the standby loop's battery-gauge branch and its `power_off()` on battery idle
* the interaction between `hold_t` and the gauge display
* whether this unit's bootloader scrubs `RESETREAS` (§ the battery-wake comment
  in main.c) — undeterminable from source
* that `controls_init()` has run before the gate loop's first `ladder_read()`

---

## 7. Definition of complete

Stage 0 is **not** complete, safe, proven or ready to build on because CI is
green. It is complete when §5 has been run on an SP-1 and the measured column is
filled in.

Until then, every row in §6 with ❌ in the hardware columns is **unproven**, and
that includes every requirement that depends on physical controls, ADC
behaviour, real timing, battery wake, charge-standby wake, or the real
`power_off()` path.
