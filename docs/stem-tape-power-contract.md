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

### 1.0 Release-to-rearm

**A single uninterrupted FUNCTION press cannot both power the device on and
later power it off. The press that causes ON must be physically released before
any OFF transaction can begin.**

```
OFF
 → FUNCTION down
 → 2.000 s
 → ON, shutdown DISARMED
 → FUNCTION still down
 → remains ON, indefinitely
 → FUNCTION RELEASED
 → shutdown ARMED
 → FUNCTION down again
 → 5.000 s
 → OFF
```

This is part of the contract, not a timing workaround. Without it the two
thresholds sit on one clock with no state between them, and seven seconds under
one finger produces two transitions.

**What arms shutdown, and it is the only thing that does:** `st_pwr_service()`
observing FUNCTION physically up. There is exactly one `off_armed = true` in the
codebase, it sits inside `if (!in->fn_down)`, and **gate E-4 asserts both the
count and the position**. Neither initialiser grants it — `st_pwr_init_on()` and
`st_pwr_init_off()` both start disarmed — so no reset, re-init, feature
consumption, combo flag, transport state or dispatcher state can fake a release,
because none of them can reach that line. A boot that happens to occur under a
held finger owes that finger's release exactly like a boot caused by one.

**The arming state is not "prior state influencing a transaction"**, and the
distinction matters because the rule below forbids exactly that. The rising edge
still forgets everything. Arming does not shorten, lengthen, pause or poison a
transaction — it decides whether a new OFF transaction may exist at all, which
is a property of *the press*, not of any gesture that preceded it.

**One thing st54 did that st59 does not:** st54 spun in the boot path
(`while (pwr_pressed())`) waiting for the button to come up. That stalled the
boot entirely under a held finger — dark LEDs, no codec, no audio, until it
lifted — which is not "remains on". The spin is removed; the rule it stood in
for is now structural. A lone held FUNCTION selects nothing in the dispatcher
(every FUNCTION gesture needs a second control), and `power_hold_service()` runs
above all of them regardless. **This is a real behavioural change to the boot
path and it is on the acceptance list (ON-11/ON-12).**

### 1.1 The transaction rule

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

### 1.2 Idle auto-shutdown — playback is active use

```
STOPPED / inactive, untouched, 300.000 s  →  OFF
```

**Idle is a statement about the instrument, not about the user.** Stem Tape can
be used like an iPod and a song can easily run past five minutes. A timer
measuring "time since last human input" would switch the device off in the
middle of a seven-minute song — a worse failure than the one it prevents.

```
idle = the transport is not traversing the song
       AND no intentional physical control is being used
```

**`transport_active` is pinned, by gate E-6, to exactly:**

```c
in.transport_active = (g_playing != 0) || st_inertia_moving(&s_stem_inertia);
```

*The reel is turning.* `g_playing` is the transport request;
`st_inertia_moving()` covers the spin-down after a STOP, which is audible,
pitched audio read from the tape and therefore still playback — without it the
idle clock would start several hundred milliseconds early, mid-sound.

This is one expression rather than a list of modes **because every mode that
manipulates the song drives the same reel**: normal playback, loop, reverse,
slow, pitch/varispeed, FX over live playback, solo/mute while playing, and
every future master scratch, isolated stem scratch, scrub and shuttle. None of
them needs a case here and none of them can be forgotten — a transport mode
that did not move the reel would not produce audio either.

**Rejected definitions, and why.** "The audio callback exists" and "the
streamer thread is running" are both true forever and would disable the idle
shutdown entirely. Gate E-6 fails the build if the expression stops naming both
`g_playing` and `st_inertia_moving`.

**What is not use.** LED animation, meter animation, housekeeping, diagnostics,
counters, background USB servicing, background eMMC work, streamer bookkeeping
while stopped, a feature flag merely remaining set, stale combo state, stale
gesture ownership, stale reverse/FX/solo state with nothing playing. None of
these has a path into the decision: `st_pwr_gov_in_t` has six fields — three
physical rails, a fader reading, `transport_active`, and a clock.

**What resets the idle clock** (when the transport is inactive): FUNCTION,
PLAY, Track 1–4, VOL ±, rocker FWD/RWD, FX chord activity, intentional movement
of any of the four faders. Reset, never pause: when use stops, a full fresh
300.000 s is required.

### 1.3 The three mechanisms are independent

| | Qualified by | Never reads |
|---|---|---|
| **A** wake, 2.000 s | FUNCTION + the control map | transport, idle |
| **B** manual, 5.000 s | FUNCTION + the control map + `off_armed` | transport, idle |
| **C** idle, 300.000 s | transport + the control map + `device_on` | `off_armed`, feature flags |

They meet in exactly one place: `st_pwr_gov_service()`'s final
`power_off_request = off_due || idle_off_due`.

`st_pwr_in_t` **has no transport field** and `st_pwr_hold.c` never mentions the
transport — gate E-5 fails the build otherwise. That is the structural
guarantee that playback can never suppress, extend or fake the manual escape
hatch, which would be the st55 failure in new clothes. In the other direction,
gate E-7 asserts the idle verdict never reads `off_armed`, so a wake press held
down cannot disable the backstop.

**D — bootloader recovery** (Track 1 + Track 4 + USB) runs before this image is
entered and is unreachable from all of the above.

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
| **idle** | `ST_PWR_IDLE_MS` | **300000 ms** | 37,500 |

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

**It stays labelled ESTIMATED until M3 is captured on an SP-1.** No amount of
host testing, mutation testing or CI can promote it: F7 and F8 prove the
detector behaves correctly *given* a threshold, not that 16 is the right
threshold. A number that separates a hand from the noise floor can only come
from the noise floor.

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
| ON-11 | turn on, then **keep holding FUNCTION** for 5 s, 10 s, 30 s | stays on; boot completes normally underneath the held finger | |
| ON-12 | during ON-11, watch the LEDs from the moment of wake | the instrument boots (LEDs, audio) while held — **st54 stalled here until release** | |
| ON-13 | release after ON-11, then hold 5.000 s | powers off — the release re-armed it | |
| ON-14 | tap FUNCTION several times, then hold 2.000 s and keep holding 10 s | wakes once, does **not** then power off | |
| ON-15 | watchdog/fault reboot, FUNCTION untouched, then hold 5.000 s | powers off — the first pass with the button up armed it | |

ON-11 through ON-15 are the **release-to-rearm** rule (§1.0). ON-12 is the one
deliberate behavioural change from st54 and the one to watch for surprises:
booting under a held button was previously impossible.

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

### 5.2b Idle auto-shutdown — the section that needs a stopwatch and patience

| # | Action | Required | Measured |
|---|---|---|---|
| ID-1 | stop the transport, do not touch anything | powers off after ~5:00 — **record the actual time** | |
| ID-2 | play a song continuously past 5:00 | does **not** power off | |
| ID-3 | play a song longer than 7 minutes end to end | plays to the end | |
| ID-4 | loop continuously past 5:00 | does not power off | |
| ID-5 | reverse playback past 5:00 | does not power off | |
| ID-6 | slow mode past 5:00 | does not power off | |
| ID-7 | pitched/varispeed past 5:00 | does not power off | |
| ID-8 | FX engaged over live playback past 5:00 | does not power off | |
| ID-9 | stop playback, then wait | the 5-minute interval starts **at the stop**, not at the last button | |
| ID-10 | at ~4:50–4:59 press PLAY | timer resets; another full 5:00 required | |
| ID-11 | repeat ID-10 with a Track | resets | |
| ID-12 | repeat ID-10 with the rocker | resets | |
| ID-13 | repeat ID-10 with VOL ± | resets | |
| ID-14 | repeat ID-10 with **each** of the four faders | resets — all four | |
| ID-15 | leave the unit stopped, untouched, on a bench overnight | powers off once at ~5:00 and stays off — resting analog noise does **not** keep it awake | |
| ID-16 | stop the transport, then hold FUNCTION 5.000 s | manual shutdown still fires on time, regardless of idle elapsed | |

ID-15 is the one M3 is really about. If the fader noise floor is above
`ST_PWR_FADER_MOVE_COUNTS`, the idle timer will be reset by noise forever and
the device will never sleep — the battery-drain failure this whole stage exists
to prevent, arriving by a different road.

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

**R-1/R-2 are the only evidence that will ever exist for mechanism D in this
project.** The bootloader's source is not in this repository (§8.2), so its
behaviour cannot be source-verified here — it is owner-confirmed. These two runs
are confirmation of *existing* recovery behaviour, not a test of anything st60
added; st60 adds no bootloader-facing code at all.

---

## 6. Proof matrix — what is actually proven, and how

| Requirement | Host | Mutation | ARM build | Structural gate | HW measured | HW behaviour |
|---|---|---|---|---|---|---|
| 5.000 s exact threshold | ✅ 1 ms rig | ✅ M-L | ✅ | — | ❌ | ❌ |
| 2.000 s exact threshold | ✅ | ✅ M-L | ✅ | — | ❌ | ❌ |
| release resets at any point | ✅ 7 probes | ✅ M-I | ✅ | — | ❌ | ❌ |
| other control resets completely | ✅ | ✅ M-J | ✅ | — | ❌ | ❌ |
| fresh full hold required after | ✅ | ✅ M-I | ✅ | — | ❌ | ❌ |
| 11 chords held 10 s never fire | ✅ | ✅ M-J | ✅ | — | ❌ | ❌ |
| 12 feature states still power off | ✅ | — | ✅ | — | ❌ | ❌ |
| no feature input can reach the decision | ✅ (signature) | — | ✅ | ✅ E-3 | n/a | n/a |
| AIN0 (PLAY, Tracks) resets the timer | ✅ F2/F3 | ✅ M-E | ✅ | — | ❌ | ❌ |
| **AIN1 (VOL, rocker, FX) resets the timer** | ✅ **F12** | ✅ M-D | ✅ | — | ❌ | ❌ |
| fader movement resets the timer | ✅ F8 | ✅ M-F | ✅ | — | **❌ M3** | ❌ |
| rising edge forgets everything | ✅ F9 | ✅ M-A/M-B | ✅ | — | ❌ | ❌ |
| stale fader cannot delay a hold | ✅ F1 | ✅ M-G | ✅ | — | ❌ | ❌ |
| **faders sampled only inside a hold** | ✅ **F13** | ✅ M-C | ✅ | — | ❌ | ❌ |
| single-sample phantom cannot reset | ✅ F6 | — *(see below)* | ✅ | — | ❌ | ❌ |
| **settle depth, both directions** | ✅ **F14** | ✅ M-H2 | ✅ | — | ❌ | ❌ |
| sub-threshold noise never blocks | ✅ F7 | ✅ M-M | ✅ | — | **❌ M3** | ❌ |
| real fader movement does block | ✅ F8 | ✅ M-F | ✅ | — | **❌ M3** | ❌ |
| ADC failure is not activity | ✅ F11 | — | ✅ | — | ❌ | ❌ |
| **one press, at most one transition** | ✅ **A3/A7** | ✅ M-N | ✅ | ✅ **E-4** | ❌ | ❌ |
| **the wake press owes a release** | ✅ **A4** | ✅ M-N | ✅ | ✅ E-4 | ❌ | ❌ |
| **an earlier tap cannot pre-arm the wake** | ✅ **A11** | ✅ M-O | ✅ | ✅ E-4 | ❌ | ❌ |
| **only a physical release arms shutdown** | ✅ **A9** | ✅ M-P/M-Q | ✅ | ✅ E-4 | ❌ | ❌ |
| **no software reset can fake the release** | ✅ **A9(b)** | ✅ M-Q | ✅ | ✅ E-4 | n/a | n/a |
| **the second press is an ordinary 5 s hold** | ✅ **A5/A6** | ✅ M-L | ✅ | — | ❌ | ❌ |
| **both wake entries obey it identically** | ✅ **A10** | — | ✅ (same code) | — | ❌ | ❌ |
| **idle fires at exactly 300.000 s** | ✅ 1 ms rig | ✅ I-11/I-15 | ✅ | — | ❌ | ❌ |
| **playback suppresses idle (7-min song)** | ✅ 10–16 | ✅ I-1/I-3 | ✅ | ✅ E-6 | ❌ | ❌ |
| **spin-down counts as playback** | — | — | ✅ | ✅ E-6 | ❌ | ❌ |
| **every control resets idle** | ✅ 3–8 | ✅ I-4/5/6/7 | ✅ | — | ❌ | ❌ |
| **background activity does not** | ✅ 17–21 | ✅ I-2 | ✅ | — | ❌ | ❌ |
| **idle never banks partial credit** | ✅ 9 | ✅ I-11 | ✅ | — | ❌ | ❌ |
| **wake discards historical idle** | ✅ wake | — *(equivalent, see below)* | ✅ | — | ❌ | ❌ |
| **idle inert while the device is OFF** | ✅ off | ✅ I-14 | ✅ | — | ❌ | ❌ |
| **transport cannot reach the manual hold** | ✅ 24c | — | ✅ | ✅ **E-5** | ❌ | ❌ |
| **idle does not read `off_armed`** | — *(unreachable, see below)* | — | ✅ | ✅ **E-7** | n/a | n/a |
| **either route alone reaches power_off()** | ✅ OR | ✅ I-12/I-13 | ✅ | — | ❌ | ❌ |
| `power_off()` absent from gated branch | — | — | ✅ | ✅ E-1 | n/a | n/a |
| service above every dispatcher | — | — | ✅ | ✅ E-2 | n/a | n/a |
| **ADC/ladder decode on real rails** | — | — | — | — | **❌** | **❌** |
| **fader noise floor (M3)** | — | — | — | — | **❌** | **❌** |
| **actual wake timing** | — | — | — | — | **❌** | **❌** |
| **actual shutdown timing** | — | — | — | — | **❌** | **❌** |
| **the real `power_off()` path** | — | — | — | — | **❌** | **❌** |
| **battery-wake vs standby parity** | — | — | ✅ (same code) | — | **❌** | **❌** |
| **bootloader independence** | — | — | — | — | **❌** | **❌** |

### The mutation sweep, and what it found

Thirteen mutants were applied to `st_pwr_hold.c` and run against the suite.
**Three survived the first sweep, and two of them were real gaps** — found by
mutating, not by reading:

* **M-D, deleting `|| in->ain1_active` from the control map, broke nothing.**
  Every failure-injection case interrupted its hold with AIN0; `svc_run()`'s
  `ain1` parameter was never once passed `true`. The AIN1 requirement — the one
  the device owner specifically insisted on, because FUNCTION + rocker is a real
  performance interaction — was asserted only against `st_pwr_hold_tick()`,
  which cannot see how the service assembles its inputs. **F12 added.**
* **M-C, sampling faders outside the transaction, broke nothing**, because
  `main.c` passes `fader_raw = -1` with FUNCTION up and the harness copied that.
  The module's own guarantee rested on the caller's discipline. **F13 added.**
* **M-H, `ST_PWR_SETTLE_PASSES` → 1, is an equivalent mutant.** The tick's first
  disagreeing sample lands in the `else` branch and sets `cand_n = 1` without
  testing the threshold, so the effective depth is `max(2, N)`. Setting the
  constant to 1 genuinely changes nothing. Raising it does: **M-H2 (→ 3) is
  killed by F14**, which now pins the depth in both directions.

### The release-to-rearm sweep (st59)

Six further mutants target the guard specifically. **One survived the first
sweep, and it was a real gap:**

* **M-O, deleting the disarm at the ON transition, broke nothing.** Every A-case
  began from a fresh `st_pwr_init_off()` and never released FUNCTION before the
  successful hold, so `off_armed` was still false from initialisation and the
  missing line could not be observed. It is observable by the most ordinary
  gesture there is: tap FUNCTION (that release arms shutdown), then hold
  properly — without the disarm, the press that wakes the instrument switches it
  off three seconds later. **A11 added**, and it kills M-O.
* **M-N** (drop the `off_armed` guard from `off_due`) → killed, 9 checks. This is
  the mutation the contract asks for by name: with it, a continuous hold becomes
  capable of ON → OFF.
* **M-P** (arm on every pass rather than on release) → killed, 15.
* **M-Q** (an initialiser grants the arm) → killed, 3.
* **M-S** (`on_due` as a level rather than an edge) → killed, 21.
* **M-R** (make `off_due` reachable while the device is OFF) → **survives, and is
  equivalent**: the transition disarms in the same pass, so `off_due` is false
  either way, and from the next pass the device is ON and the `else` runs
  regardless. The `else` is defence in depth, not observable behaviour.

Gate **E-4 is itself mutation-tested**: adding a second `off_armed = true`,
moving the assignment out of the `!fn_down` branch, and deleting the
transition's disarm each make the gate fail, and the unmutated source passes.

### The idle sweep (st60)

Sixteen mutants against `st_pwr_idle.c`. **Three survived the first sweep, and
two were real gaps in the tests:**

* **I-13, dropping the manual route from the final OR, broke nothing** — because
  the rig counted `off_due` *inside* `if (out.power_off_request)`. It never
  looked at a route unless the combined flag had already fired. The rig now
  counts each route independently and asserts
  `power_off_request == off_due || idle_off_due` on every pass of every case.
* **I-14, letting idle fire while the device is OFF, broke nothing** — no case
  sat at the wake gate for a full five minutes. In production the standby loop
  acts only on `on_due`, so it was inert; "currently inert" is how a latent
  defect is described the day before it is wired up. A six-minute
  device-off case was added.
* **I-2, dropping the transport from `in_use`**, was caught only because the
  playback cases now assert `in_use` on every pass — the reason they stay on
  must be the reel, not an accident of the timer.

**Two documented equivalent mutants, with the reachability argument rather than
a claim:**

* **I-9 / I-16, making the idle verdict depend on `off_armed`, cannot be killed
  by any input.** `off_armed` is granted by any pass with FUNCTION up; FUNCTION
  down is intentional activity and pins the idle clock at zero; so a 300,000 ms
  idle expiry requires 300,000 ms of FUNCTION being up, which necessarily armed
  shutdown on its first pass. At the moment idle expires, `off_armed` is always
  true. The independence is real and enforced — **gate E-7** asserts the idle
  verdict does not mention `off_armed` — but it is **structurally inspected, not
  host-proven.**
* **I-10, removing the idle re-init at the ON transition,** is equivalent
  because the wake press is itself intentional activity and has already pinned
  the clock. The explicit re-init stays as defence in depth: it makes the
  guarantee independent of that coincidence.

All nineteen manual-path mutants are killed or explained. The surviving-mutant list — the
honest statement of what the suite does not constrain — is **M-H and M-R**, both
documented above as equivalent.

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

---

## 8. The four mechanisms, kept separate on purpose

Three of these are application-level power *policy*. The fourth is *recovery*,
lives outside the application, and is not ours. Blurring them is how "the
device can always be switched off" stopped being true in st55.

| | Mechanism | Trigger | Purpose | Owner |
|---|---|---|---|---|
| **A** | **Wake** | FUNCTION-only, continuously 2.000 s, from OFF | deliberate startup; a single accidental press in a pocket or bag must not boot it | `st_pwr_hold.c` |
| **B** | **Manual shutdown** | after release-to-rearm, FUNCTION-only, continuously 5.000 s | deliberate user-requested shutdown; reachable from **every** application feature state | `st_pwr_hold.c` |
| **C** | **Idle shutdown** | transport inactive **and** no intentional physical activity, 300.000 s | stock-style battery protection; a secondary application-level route if the unit is abandoned | `st_pwr_idle.c` |
| **D** | **Bootloader recovery** | Track 1 + Track 4 held while USB/power is applied | recovery when the **application firmware itself** is broken | the SP-1 bootloader — **not this repository** |

A, B and C converge on one platform transition and nothing else. Their
qualification logic is disjoint — §1.3, gates E-5 and E-7. **D shares nothing
with them at all**, including the failure modes: A/B/C are all code in the
application image, so a sufficiently broken application can in principle
compromise all three at once. That is precisely why D must not be, and is not,
implemented here.

### 8.1 D is not "running in the background"

The bootloader is not a supervisor. It runs **at startup**, decides whether
recovery was requested, and then either stays in firmware-update mode or hands
control to Stem Tape and is done. While Stem Tape is running, no bootloader
code is executing and nothing is watching the application on its behalf. (A
`power_off()` → `SYSTEM_OFF` and the subsequent wake is a *fresh startup*, so
the bootloader runs again then — at startup, as always, not concurrently.)

### 8.2 What is source-verified about D, and what is not

**Source-verified, in this repository:**

* The application is **not** the reset vector.
  `firmware/boards/teenageengineering/stem_player/stem_player.dts:179-180` —
  *"TE bootloader lives below 0x20000; it loads the app at 0x20000, max size
  0xDF000"* — and `stem_player_defconfig:8`. Something else runs first, by
  construction. CI's "Image assertions (origin, bounds, …)" step checks the
  origin fail-closed.
* **No application code checks a Track 1 + Track 4 combo.** There is no such
  mask test in `main.c`.
* The **in-firmware** Track1+Track4→UF2 reset (`enter_dfu()`) was removed in
  **`e887699`, 2026-08-20 02:39 UTC** — twelve days *before* st54
  (`3b77ee7`, 2026-09-01 22:19) and st55 (`fc2d90b`, 2026-09-02 01:04). So the
  surviving recovery path is the bootloader's, and it predates the scratch
  series. The app no longer writes `GPREGRET` at all.
* **st60 adds no bootloader-facing code.** `git diff d9aedfc..HEAD` over
  `firmware/stemtape_player/src/` contains **zero** added lines matching
  `gpregret|NVIC_SystemReset|dfu|uf2|UICR|0x10001`. Every "bootloader" hit in
  that diff is a comment.

**NOT source-verified, and it must not be presented as if it were:** that the
bootloader actually scans Track 1 + Track 4 at power-up. **The bootloader's
source is not in this repository** — `firmware/boards/teenageengineering/`
contains board definitions only, and no bootloader source or image is vendored.
The behaviour rests on the **device owner's confirmed procedure**, recorded in
`3ce7d92` and `README.md:119-128`, corroborated by the distinguishing cue: the
bootloader lights **one** track LED, whereas the removed `enter_dfu()` lit
**all four** — two different code paths. Acceptance **R-1 / R-2** is what
closes this, and until then D is *owner-confirmed, not source-verified*.

---

## 9. Would st60's idle shutdown have recovered st55? **No.**

Asked and answered before flashing, because the answer determines whether C is
a battery-protection feature or a fault backstop. It is the former.

### 9.1 `transport_active` would have stayed true

**`st_inertia_moving()` is not an independent liveness signal.** It is a pure
function of `g_playing`:

* `main.c:4401-4405` (st60; identical at `4552-4555` in st55) —
  `if (g_playing) st_inertia_play(); else st_inertia_stop();`
* `st_inertia_play()` drives SPINUP → RUNNING and **stays** RUNNING
  (`st_inertia.c:91-99`, `:158-161`). Only `st_inertia_stop()` reaches STOPPED
  (`:104-112`, `:168-171`), and it is called only while `g_playing == 0`.
* `st_inertia_moving()` is `state != ST_INERTIA_STOPPED` (`st_inertia.h:139`).

So `transport_active ≡ g_playing`, plus a bounded tail of at most
`ST_INERTIA_SPINDOWN_MS` = 600 ms (`st_inertia.h:74`). It cannot latch true on
its own.

**And `g_playing` is a latch, not a liveness signal.** In st55 it has exactly
three writers: the PLAY toggle (`main.c:9075`), the loop-restart path (`:4973`),
and the transfer pause (`:6120`). Nothing in the streamer, the mailbox, the
underrun path, `st_stream_*` or the scratch code writes it — `st_scratch.c`'s
only match for "inertia" is a comment on line 151, and `st_ctl.c` has zero
references to either symbol.

The st55 stranded transport was in **UNDERRUN livelock**, producing no audio at
all — postmortem §2.8: *"the head cannot advance because it has no sector, and
it has no sector because the address will not hold still."* Through all of that,
`g_playing` stayed exactly what the PLAY latch said. The user was scratching a
playing song, so it was 1.

> **`transport_active` means "the transport was commanded to run", not "the
> transport is running."** For C's actual job — never cut off a seven-minute
> song — that is exactly the right question. For fault recovery it is exactly
> the wrong one: it is true *precisely* when the transport has stalled while
> still commanded to run.

### 9.2 And FUNCTION was being held, which is activity

Independently of the above. The stranding was `function_consumed` re-asserted
continuously by `scratch_service()` for as long as a target was latched (§2.1),
and the target latched for the whole FUNCTION press with no way to disown it
(§2.8) — so every escape attempt was another FUNCTION press, and §2.4's phantom
ADC activity re-latched the scratch each time.

In st60, `fn_down` is intentional physical activity and pins the idle clock at
zero. Even with `g_playing == 0`, every attempt to escape would have reset the
five minutes.

### 9.3 Nor would the existing hardware watchdog have caught it

Worth stating so it is not assumed. st55 did not hang: the control loop kept
running — that is how the phantom Track presses and volume changes of §2.2
happened at all — so `feed_wdt()` kept being called and the WDT kept being
satisfied. It was a **livelock in data flow**, not a stalled CPU.

### 9.4 The distinction this forces, and the line not to cross

| | Question it answers | Correct input | Failure it catches |
|---|---|---|---|
| **C, idle shutdown** | *Is the instrument being **used**?* | `g_playing` — the command | an abandoned device flattening its battery |
| **A future fault backstop** | *Is the instrument **working**?* | a **liveness** signal — frames actually delivered to I2S, `st_stream_sector_ready()` progress, the mailbox consumer index advancing | a commanded-but-stalled transport |

`g_playing` is structurally incapable of answering the second question, and no
tuning of C can make it do so.

**Do not merge them.** Any liveness signal is legitimately false during a real
pause, a momentary underrun or a slow seek; wiring one into C would trade the
long-song guarantee — the whole reason §1.2 exists — for a fault case that has
a proper, separate answer. **No watchdog is being implemented now.** This
section exists so the next person does not reach for C when they mean the other
thing.
