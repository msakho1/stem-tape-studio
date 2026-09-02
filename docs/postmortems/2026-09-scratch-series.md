# Postmortem — the scratch series (st54/st55), and how it trapped an SP-1 powered on

**Status:** closed by full revert of the scratch series.
**Baseline restored to:** **`st54`** — the last build confirmed working on
hardware. Tree state `d9aedfc`; binary `44ed7885…ec9c`, 115,148 B. See §4.1.
**Failed build:** `st55` — series head `3558c91`.
**Severity:** **release-safety defect.** A performance gesture made the device's
only software power-off path unreachable. The unit could not be switched off and
would have stayed on until the battery drained. It also produced continuous
audio corruption and permanently displaced a stem's playhead.

> **This is a release-safety defect, not an acceptable consequence of
> experimental development.** The SP-1 it happened on *was* the dedicated test
> device. That is not a mitigation and it is not the finding. Being the test
> device does not make it acceptable for firmware to strand it. The failure is
> that an experimental build was allowed to disable the test device's own
> recovery and power-off path — the exact thing that makes a device safe to
> experiment on. A development unit must always be able to escape bad firmware
> **immediately**, without waiting for battery depletion. §3.0 states that as a
> rule with priority over every feature; §4 makes it a precondition of producing
> a flashable artifact at all.

This document exists so the same class of failure is not reachable again. It is
written to be read by someone who was not there.

---

## 0. Corrections to the record

Four things I previously told you were wrong, and one gate turns out to have
been protecting the bug. Correcting them here because the rest of the analysis
depends on getting them right.

0. **The baseline is st54, not st53.** I reverted one commit too far. `st54`
   (`3b77ee7`, tree `d9aedfc`) is the last build that was flashed and confirmed
   working; `st53` (`bd8114b`) is merely the last commit before the meter fix.
   The difference is real firmware — st54 corrects the Track meter's brightness
   constants, which were still 24-bit against 16-bit samples and made the LEDs
   read 256× low. Restoring to st53 would have shipped you a known display bug
   in the name of safety. The tree is now `d9aedfc` + this document, build tag
   `st54`, and §4.1 pins its binary identity. **I should have asked which build
   you last confirmed rather than assuming the commit before the series head.**

1. **`st_stream_set_reverse()` does not flush residency.** My revert commit
   message says direction changes "re-primed the whole ring". That is false.
   `st_stem_stream.c:95-133` explicitly preserves `song_frame` and
   `ready_sector`, and touches residency in exactly one case — turning reverse
   *on* while parked at `END_OF_SONG`. The real residency failure is one level
   up, in main.c's *prefetch request address*, and it is described properly in
   §2.5. The primitive was not the problem; how the gesture drove it was.

2. **st55 did not introduce the `function_consumed` gate on power-off.** That
   gate is st53's, at `main.c:9807`. What st55 introduced was a *third producer*
   of `function_consumed` that asserts it continuously rather than on a
   completed gesture. The gate was survivable with two discrete producers and
   fatal with a continuous one. §2.1.

3. **A CI gate was pinning the faulty construct.**
   `.github/scripts/stemtape_player_stem_playback_wiring_check.py:453` requires
   the literal source line `"if (pwr_pressed() && !g_stem_ctl_out.function_consumed) {"`
   to exist. It was added to protect the *loop latch* ("which is what makes the
   loop latch reachable at all"). Nobody ever wrote the complementary gate
   proving power-off stays reachable. So CI would have gone **red if I had fixed
   the bug** and stayed green while the bug shipped. That is the single most
   important process finding in this document.

---

## 1. Every change the series introduced

The scratch series proper is **`d9aedfc..3558c91`** — eleven commits, 12 files,
+3395 / −58. The wider range `bd8114b..3558c91` (24 files, +4535 / −101) also
contains st54 and two doc commits, which are **not** part of the series, are not
implicated in any failure, and are in the restored baseline. They are
inventoried in §1.7 so the boundary is unambiguous.

### 1.1 `src/st_scratch.h` / `src/st_scratch.c` — NEW, 621 lines

| | |
|---|---|
| **What changed** | A new pure module: a signed-rate integrator (`st_scratch_t` = `rate_q16`, `drive_q16`, `max_rate_q16`, `engaged`, `coasting`) with `begin`/`set_drive`/`tick`/`release`/`coast`, two control mappings (`drive_from_rocker`, `drive_from_fader`), and a compile-time velocity clamp derived from the measured read cost. |
| **Why** | To make reverse, scrub and scratch one state machine at different gesture durations, per your explicit design instruction — no timeout, no mode flag, no separate engines. |
| **Expected** | Drive in, signed rate out, clamped so the eMMC can never be over-drawn. Continuous zero crossing. |
| **Actual on hardware** | The module itself behaved. Every failure below is in what consumed it, not in it. Its *clamp derivation* was nonetheless wrong in premise — see §2.5. |
| **Safety-critical paths touched** | Audio transport (indirectly — it produces the rate), streamer scheduling (its clamp is the starvation bound). |
| **Why tests missed it** | They didn't, for the arithmetic. `test_scratch.c` (827 lines) found five real bugs pre-flash. What it could not find is that its central premise — "oscillating inside the resident ring costs nothing" — was false, because it modelled residency instead of using it. §2.5. |

Constants it introduced, and their provenance:

| Constant | Value | Derived from | Measured? |
|---|---|---|---|
| `ST_SCRATCH_RC_FIXED_US` / `_PER_BLOCK_Q8` | 649 µs, 158.4 µs/blk | st32 read-cost sweep, 24 reads/size | **yes** |
| `ST_SCRATCH_BATCH_BLOCKS` / `_US` | 12 blocks, 2550 µs | R=3 groups × 4 blocks, via the fit | derived |
| `ST_SCRATCH_BATCH_COVER_US` | 31,875 µs | R × `ST_LAT_SECTOR_US` | derived |
| `ST_SCRATCH_BUDGET_PCT` | 85% | `100 − ST_LAT_MARGIN_PCT` | inherited |
| `ST_SCRATCH_EMMC_MAX_MASTER_Q16` | 2.656× | the duty model | derived |
| `ST_SCRATCH_EMMC_MAX_STEM_Q16` | 7.625× | the duty model | derived |
| `ST_SCRATCH_RENDER_MAX_Q16` | 2.000× | `ST_RS_RATE_MAX` — interpolator span | structural |
| **effective clamp, both targets** | **2.000×** | min of the two | derived |
| `ST_SCRATCH_FREE_WINDOW_US` | 53,125 µs | (G−1) × sector | derived — **and the premise is wrong, §2.5** |
| `ST_SCRATCH_ACCEL_MS` | 80 ms | feel, by analogy | **no** |
| `ST_SCRATCH_DECEL_MS` | 50 ms | feel, by analogy | **no** |
| `ST_SCRATCH_FADER_DEADBAND_CPS` | 200 | control cadence (1 count/pass = 125 cps) | **no** — the header says so |
| `ST_SCRATCH_FADER_FULL_CPS` | 3700 | one fader travel per second | **no** |

Four of the numbers that decide how the feature *feels and arms* were unmeasured.
The header admitted this for the deadband and shipped anyway. That was my call
and it was wrong: an unmeasured arming threshold on a control that can
permanently displace a stem is not a "tune it later" parameter.

### 1.2 `src/st_ctl.h` / `src/st_ctl.c` — the gesture and its arbitration

**New input fields** (`st_ctl_in_t`): `rocker_dir` (int8, −1/0/+1), `fader_raw[4]`
(raw ADC counts, −1 = not sampled this pass).

**New output fields** (`st_ctl_out_t`): `scratch_active`, `scratch_target`,
`scratch_drive_q16`, `rocker_consumed`, `fader_consumed_mask`.

**New state** (`st_ctl_t`): `scr_rock_cand`, `scr_rock_n`, `scr_rock` (a 2-pass
rocker debounce, deliberately shorter than the volume path's 3),
`scr_target` (first-mover-wins, latched until FUNCTION release),
`scr_fader_prev[4]`, `scr_last_ms`.

**New function** `scratch_service()`, called **first** in `st_ctl_service()`, ahead
of every other handler. **New function** `st_ctl_scratch_end()`, called from
reset, on FUNCTION release, and when the stem song goes away.

| | |
|---|---|
| **What changed** | One handler that decides, per pass, what the hand is asking of which head, and marks the rocker / that fader / the FUNCTION press as spent. |
| **Why** | So one physical movement does exactly one thing — the rule this file already enforced for PLAY. |
| **Expected** | FUNCTION alone changes nothing (the code has an explicit early-out: *"FUNCTION IS HELD BUT NOTHING HAS MOVED YET… nothing is published and nothing is consumed"*). A rocker press claims master; a fader movement claims that stem. |
| **Actual on hardware** | The early-out never held, because "nothing has moved yet" was decided from ADC readings that were themselves moving. Once a target latched, `out->function_consumed = true` every pass until release — §2.1. |
| **Safety-critical paths touched** | **Power-off** (via `function_consumed`), **input dispatch** (it now runs above every other handler), gain/mute/solo (via the fall-through it caused), transport. |
| **Why tests missed it** | `test_ctl.c` gained 8 cases (`…rocker_is_master`, `…fader_is_one_stem`, `…first_mover_owns`, `…ends_with_function`, `…unsampled_fader`, `…suppresses_reverse_double_tap`, `…single_noisy_sample`, `…rocker_without_function`). Every one drives `st_ctl` with *synthetic, clean* inputs. `test_a_single_noisy_sample_moves_nothing` tested exactly one noisy sample — the hardware delivered a systematic, sustained bias, which is a different signal and passes that test trivially. Nothing tested `st_ctl` against recorded ADC traces, because none exist for the fader rails. |

### 1.3 `src/main.c` — five distinct changes, +381 lines

#### 1.3a The control→audio mailbox

New `g_stem_scratch_req`, one atomic word packing `{target: bits 20-23, drive:
bits 0-19}`, with `ST_SCR_PACK` / `ST_SCR_TGT` / `ST_SCR_DRIVE`. Published by
`stem_ctl_apply()` as a **level, every pass**.

- *Why:* target and drive must be read as a pair; a torn read would push the
  wrong head for one block.
- *Expected/actual:* this part worked. The packing and sign-extension are
  correct.
- *Safety-critical:* audio transport.

#### 1.3b Per-stem rate — `stem_render_run()` signature change

`uint32_t rate_q16` → `const uint32_t rate_q16[ST_PL_STEMS]`. `together` gained a
rate-equality term. `out_n` changed from *one* `st_rs_out_frames()` call using
the worst carried fraction to a **minimum over four calls**, one per head at its
own rate.

- *Why:* an isolated stem scratch moves one head while three run forward; that
  is not one number.
- *Expected:* at a shared rate every term is identical and playback stays
  bit-identical. **This held** — the full-playback gate's `0x2a737e00` hash was
  unchanged.
- *Safety-critical:* audio transport, renderer bounds.
- *Why tests missed nothing here:* they didn't. This change was correct.

#### 1.3c The apply block (~110 lines inside `stem_audio_block()`)

Statics `s_scr` (the integrator), `s_scr_owner`, `s_scr_was_rev`. Per block:
read the atomic; grab on the rising edge from the transport's current **signed**
rate; `set_drive` + `tick` while live; `coast` toward the signed transport rate
when not; then split the signed rate into magnitude (`stem_rate_q16[sk]`) and
sign (`st_stream_set_reverse()`), applied to the owning head(s).

- *Why:* the rate must change on block boundaries and this thread has the only
  exact clock.
- *Expected:* master moves four heads phase-locked; a stem gesture moves one.
- *Actual:* **§2.6 (no real transport movement), §2.7 (permanent stem drift),
  §2.9 (restart from the top).** This is the single most damaging change in the
  series.
- *Safety-critical:* audio transport, residency, transport reset, streamer
  scheduling.
- *Why tests missed it:* `test_scratch_apply.c` compiles **this exact source
  text** (extracted at build time into `.inc` files, so it cannot go stale) — a
  good technique that proved the arithmetic. But it compiles it against stubs
  where `g_stem_stream[]` is a bare array with no producer, no mailbox, no ring,
  **and nothing that can ever return `UNDERRUN`**. In a world with no
  starvation, "four heads at one rate stay locked" is true by construction. Its
  own header states two limits honestly ("cannot prove the block sits in the
  right place… or that it runs once per block") and omits the three that
  mattered.

#### 1.3d The ADC schedule — the change that started the cascade

```c
if (pwr_pressed() && stem_ctl) {
        if (tgt < ST_PL_STEMS)            ci.fader_raw[tgt] = ladder_read(…);
        else if (tgt == ST_CTL_SCRATCH_NONE)
                for (fk = 0; fk < 4; fk++) ci.fader_raw[fk] = ladder_read(…);
}
```

Plus, in the round-robin volume read, `int fv = claimed ? -1 : ladder_read(…)`.

- *Why:* a hand on a record sampled at the round-robin's 31 Hz is unusable —
  most of a scratch falls between two reads.
- *Expected:* "The cost is bounded and brief: four blocking ADC reads instead of
  one, only while FUNCTION is down and only until the hand commits." That
  sentence is in the shipped source and it is wrong in three ways at once: the
  cost is not one-for-four, it is not brief, and the hand never has to commit.
- *Actual:* §2.3, §2.4.
- *Safety-critical:* **streamer scheduling**, **input dispatch**.
- *Why tests missed it:* nothing in CI models the control thread's timing. There
  is no host harness for the main loop's ADC cadence and no CPU budget assertion
  of any kind. The only artefact that knew the answer was a **comment**, quoted
  in §2.3.

#### 1.3e Three suppressions in the inherited Tape Looper path

`!g_stem_ctl_out.rocker_consumed` on the CHOP divider (~line 10282) and on the
varispeed `dir` (~line 11123); `fader_consumed_mask` on the volume round-robin.

- *Why:* one movement, one action — a shuttle must not also transpose the song
  or fade the stem.
- *Expected/actual:* these were correct in intent, and irrelevant in practice
  because of §2.2 — the far larger fall-through went unnoticed underneath them.
- *Safety-critical:* gain, pitch.

### 1.4 `CMakeLists.txt`

`src/st_scratch.c` added. Added *late*, after a CI link failure whose commit
message claimed "nothing in the firmware calls any of this yet" while the same
commit called it. That failure is why the link-closure gate was moved from step
42 to step 4 (§1.5) — the only structural CI improvement in the whole series,
and it is worth keeping.

### 1.5 `.github/workflows/firmware.yml`

- **moved:** "Link closure" from step 42 → step 4, so it runs *before* the
  Zephyr build rather than after. (Keep.)
- **added:** "SIGNED-HEAD VELOCITY CLAMP" test step.
- **added:** "SCRATCH APPLIED — main.c's own block, compiled on stubs" step,
  driving `.github/scripts/stemtape_player_extract_scratch_apply.py`.

### 1.6 `.github/scripts/stemtape_player_extract_scratch_apply.py` — NEW

Lifts main.c's own apply and loop-wrap blocks between markers into `.inc` files
so the test compiles production source rather than a copy; strips comments before
checking the block still contains the required calls (a guard added after an
earlier version passed a gutted block). **The technique is sound and should come
back.** What it cannot do is supply a realistic environment, which is the actual
gap.

### 1.7 What is NOT part of the series — the baseline boundary

These sit between `bd8114b` and the first scratch commit. They are **in the
restored baseline** and none is implicated in any failure. Listed because my
first revert removed them by mistake (§0.0) and the boundary needs to be
unambiguous.

| Commit | What | In baseline? | Implicated? |
|---|---|---|---|
| `b3914fb` | v1.3 companion Lovable prompt (doc) | yes | No |
| **`3b77ee7` = st54** | `st_stem_meter.h` constants derived from `ST11_PCM_BIT_DEPTH` instead of hard-coded 24-bit; `glide()` given a minimum step of 1 so a proportional decay terminates. **The firmware change that makes st54 st54.** | **yes** | No — a bug *fix*. Without it the Track LEDs read 256× low. |
| `74c6561` | v1.3 companion verification doc | yes | No |
| `d9aedfc` | companion index record as a cross-party fixture + `test_stix_companion_produced_record` | yes | No |
| — | comment-only edits in `st_ab_session.c`, `st_fx.h`, `st_stem_mix.c` (part of st54's domain cleanup) | yes | No |

`d9aedfc` and `3b77ee7` produce the **same firmware image** — everything after
st54 in this list is docs, a fixture and a host test. So "the st54 binary" and
"the `d9aedfc` binary" are one artifact, identified in §4.1.

---

## 2. The failures, reconstructed

### 2.1 Power-off became unreachable

**The mechanism, exactly.**

`main.c:9807`:

```c
if (pwr_pressed() && !g_stem_ctl_out.function_consumed) {
        ctl_flush = 1;
        if (press_start < 0) press_start = k_uptime_get();
        …
        int64_t held = k_uptime_get() - press_start;
        if (held >= HOLD_MS_TO_OFF) power_off();   /* never returns */
        …
        k_msleep(25); continue;
}
if (press_start >= 0) {          /* "just released" */
        …
        press_start = -1;        /* main.c:10170 */
        …
}
```

`press_start` is the *only* thing that measures the hold, and it lives inside the
guarded branch. When `function_consumed` goes true while the button is still
physically down:

1. the branch is skipped, so `held` is never computed and `power_off()` is
   unreachable;
2. control falls into the **"just released"** branch even though nothing was
   released, and `press_start = -1` **destroys the timer**. It does not pause —
   even if the consumption later cleared, the 2.5 s would restart from zero;
3. execution then continues past the branch into the ordinary control decode,
   which in the baseline is *never* reached with FUNCTION down because of the `continue`
   at 10069. That fall-through is §2.2.

**Every path that can consume FUNCTION.** `c->fn_consumed` is set in exactly
three places, and cleared in one (`if (!in->function_down) c->fn_consumed = false;`,
`st_ctl.c:318`):

| Producer | Where | Shape | Reachable by holding FUNCTION alone? |
|---|---|---|---|
| Loop latch (`ST_LOOP_ACT_LATCH`) | `st_ctl.c:263` | one-shot, at the end of a completed FUNCTION+PLAY gesture | **No** — needs PLAY |
| Reverse double-tap, second tap | `st_ctl.c:175` | one-shot, at the end of a completed FUNCTION+Track×2 gesture | **No** — needs a Track button |
| **`scratch_service()`** | st55, `st_ctl.c` | **continuous** — re-asserted every pass for as long as a target is latched | **Yes** — §2.7 |

The first two are *terminal events on a deliberate chord*. A player holding
FUNCTION and nothing else could never reach either. The third asserts on a
*continuous, analog* condition, and the condition could be met by noise.

**Why a performance gesture was allowed to suppress power-off at all.** Because
`function_consumed` was a single, untyped boolean used for two entirely different
jobs: "don't let the inherited Looper *also* interpret this press" (correct, and
what it was built for) and "don't run the shutdown timer" (an accident of the
timer living inside the same `if`). Nothing in the type system, the tests, or
the reviewer's head distinguished *musical* suppression from *safety*
suppression. The fix is not a better predicate — it is that the shutdown timer
must not be inside anything a feature can gate.

### 2.2 FUNCTION triggered unrelated behaviour — vocal solo, volume changes

**Full input path.**

```
FUNCTION (GPIO)  ──────────────────────────────────► pwr_pressed()
AIN0 ladder ── ladder_read() ──► st_trk_raw ──► ci.ladder_raw
                                                     │
                                              st_ladder_update()          3 agreeing reads (~24 ms)
                                                     │                    hysteresis ±6, guard zones
                                              st_ladder_mask() ──► out->track_mask
                                                     │
                                              stem_ctl_apply()  main.c:8779
                                                     │
                                              trk[k].solo = bit          ← EVERY PASS, NO FUNCTION GATE
                                                     │
                                              stem_channels[s].solo  main.c:3246 ──► mixer
AIN1 ladder ── ladder_read() ──► st_vol_raw ──► ci.vol_dir / vcommit ──► varispeed, loop division
AIN3/6/2/7  ── ladder_read() ──► fader round-robin ──► trk[fi].vol_q8
```

Two separate leaks, and it matters which is which:

**(a) The solo did not come from a fall-through.** `stem_ctl_apply()` is called
at `main.c:9797`, *above* the FUNCTION branch, in both the baseline and st55, and it
writes `trk[k].solo` from `track_mask` unconditionally. That path is identical
in both builds. So the phantom solo means AIN0 genuinely read a Track band with
no finger on a Track button — three consecutive in-band readings.

The measured band table (`st_ladder.c:23`) makes the prediction specific:

| | raw |
|---|---|
| idle | ≤ 110 |
| **T1 (Vocal, `ST11_STEM_VOCAL = 0`)** | **180 – 230** |
| T2 | 375 – 425 |

Track 1 is the **lowest band and the one nearest idle**. An upward excursion of
70–120 counts — 1.7–2.9% of a 12-bit full scale — sustained across three passes
decodes as exactly one thing: Vocal. You reported the vocal. The nearest-band
prediction and the symptom agree, which is why I attribute this to rail coupling
from the new ADC schedule (§2.4) rather than to a logic fall-through.

I want to be precise about confidence: the *decode* is source-certain, the
*coupling magnitude* is inferred. I have not measured the AIN0 rail under the
st55 sampling schedule and cannot, from source alone, prove the excursion
reached 70 counts. §5 names the capture that would settle it.

**(b) The volume changes did come from a fall-through**, and this one is
source-certain. With `function_consumed` set (§2.1), the `continue` at 10069 is
never reached, so the pass runs on into:

- the fader round-robin (`trk[fi].vol_q8 = …`) — for the three faders the
  gesture had *not* claimed, since only the claimed one is masked. Any hand
  resting on a fader while shuttling writes its volume.
- the varispeed rocker `dir` — masked only by `rocker_consumed`, which is set
  only when the *master* gesture owns it. During an **isolated stem** gesture
  `rocker_consumed` is false, so the rocker still transposes.
- `ladder_read(&adc_ladder[LAD_TRACKS])` at line 10168 — an *extra* conversion,
  now firing on every spurious "release".

So: scratch input leaked into normal handlers not because the arbitration was
incomplete — `rocker_consumed` and `fader_consumed_mask` were both correct — but
because the arbitration was placed in `st_ctl`, one layer above the inherited
Looper decode, **and the mechanism that was supposed to stop the Looper decode
from running at all (`continue`) was the same mechanism the gesture had just
disabled.** The arbitration and the shutdown timer were coupled through one
boolean, and breaking one broke the other.

### 2.3 Extra ADC sampling starved playback

**The new reads.**

| Situation | `ladder_read()` calls per pass | conversions (2 each) |
|---|---|---|
| baseline, ordinary play | TRACKS, VOL, 1 round-robin fader | 3 → **6** |
| baseline, **FUNCTION held** | TRACKS, VOL only — `continue` skips the rest | 2 → **4** |
| st55, FUNCTION held, no owner | TRACKS, VOL, **4 faders**, 1 round-robin, +1 release-path | 7–8 → **14–16** |
| st55, FUNCTION held, owner latched | TRACKS, VOL, 1 fader, 1 round-robin, +1 | 4–5 → **8–10** |

**st55 made the most expensive situation the one that occurs while the player is
performing, and it made it 3.5–4× more expensive than the baseline's, in the one
case the baseline had deliberately made cheapest.**

**Why all four were polled before an owner existed.** Because the design made the
*hardware* answer "which fader is the hand on?" instead of the *player*. There
was no explicit arm step, so the only way to detect a fader gesture was to watch
all four. That is the architectural error: an implicit gesture start forces a
polling cost you cannot bound, and a polling cost you cannot bound on this
device is a starvation cost.

**Quantifying it, honestly.** I do not have a measured microsecond figure for
`adc_read_dt()` on this build, and I am not going to invent one. What I do have
is the repo's own calibration, in a comment I read and did not apply
(`main.c:340`, present since long before this series):

> *"CAREFUL with the count: blocking ADC reads run on the main thread, which
> PREEMPTS the eMMC streamer — at 4× across 6 ladders the stolen CPU slowed the
> bit-banged card below the ~26.6 blk/s a take produces and brought back
> record-ring overflows (corrupt loops). 2× + round-robin faders keeps the main
> loop's ADC cost at the level the working builds had."*

That gives two anchors on the same axis: **24 conversions/pass = known bad,
6 = known good.** st55 sits at **14–16 during a FUNCTION hold** — more than
halfway from the working point to the point that was already proven to starve
the card. The comment names the exact consequence (streamer preemption →
throughput below what the audio path needs) and the exact symptom class you
heard.

**Why it was not budgeted before flashing.** There is no CPU or timing budget
assertion anywhere in CI, for any thread. Every other resource on this device is
gated — FLASH, RAM (`RAM budget assertion (fail-closed)`), read throughput
(`READ-COST MODEL`), residency depth — but main-loop time is not, so adding
work to the hot loop is the one change the pipeline cannot see. The scratch
clamp derivation went to considerable trouble to bound the *eMMC's* duty and
never once asked what the *control thread* was about to cost.

### 2.4 ADC activity caused phantom track presses

Every ladder is a resistor divider powered by **`BTN_COM` (P1.10)**, a single
GPIO in standard (S0S1) drive that feeds all six analog rails
(`main.c:292-313`). AIN0 (PLAY + 4 tracks), AIN1 (Vol + rocker) and AIN3/6/2/7
(the four faders) share that one driven node. Every SAADC acquisition charges
the converter's sampling capacitor **from that shared rail**, with 20 µs
acquisition at 12-bit (`stem_player.dts:109`).

Going from 4 to 14–16 conversions per pass means the rail spends far more of each
pass being loaded, with far less recovery time between acquisitions. The
perturbation is not random — it is a *systematic, periodic* bias, repeating with
the control loop. That distinction is what defeats the debounce: `st_ladder`'s
three-agreeing-reads rule and its guard zones are built to reject *uncorrelated*
noise, and its own comment says so ("only a positively in-band reading ever
names one, so guard-zone noise can never accumulate agreement"). A systematic
offset produces exactly the correlated, in-band, repeated reading the design
assumed could not happen.

**Why this wasn't accounted for.** The rail was documented — the `ladder_read()`
comment names audio and USB coupling into `BTN_COM` as the reason for 2×
oversampling. I read that comment while writing the fader change (it is 12 lines
above the code I modified) and treated it as a *throughput* warning only. It is
also a *signal integrity* warning, and the two failure modes it describes are the
two you reported. Adding converter traffic to a rail whose known problem is
coupled noise, in order to read a control on that same rail, is the mistake.

### 2.5 Scratch invalidated residency on every direction flip

**What `st_stream_set_reverse()` actually does** (`st_stem_stream.c:95-133`) —
correcting §0.1: it sets `st->reverse` and **preserves position and residency**.
It lifts exactly one terminal state, the one the new direction frees:
`!reverse && START_OF_SONG → PLAYING`, or `reverse && END_OF_SONG →` pull back to
`frames-1` **and** invalidate `ready_sector` (the one case, and it is correct —
that one *is* a position change).

**Where the flush really comes from: the prefetch request address.**

```c
if (g_stem_stream[sk].reverse)
        ahead = (needed >= ST_PL_REFILL_GROUPS) ? needed - ST_PL_REFILL_GROUPS : 0;
else
        ahead = needed + 1;
st_stem_mbox_set_requested_sector(&g_stem_mbox[sk], ahead);
```

The producer always fills an **ascending run of R groups from the requested
address**, and the ring is **direct-mapped**: sector `s` → slot `s mod G`.

With the shipped geometry **R = 3, G = 6**:

| | sectors | slots (mod 6) |
|---|---|---|
| consumer holds | `n` | `n` |
| forward read-ahead | `n+1 … n+3` | `n+1, n+2, n+3` |
| reverse read-ahead | `n−3 … n−1` | `n+3, n+4, n+5` |

The union is **7 distinct sectors over 6 slots**, and `(n−3) mod 6 == (n+3) mod 6`.
**The two directions' read-ahead regions provably alias.** They cannot both be
resident. Every sign change redirects the producer by R+1 = 4 sectors and evicts
the other direction's prefetch, costing a fresh `ST_SCRATCH_BATCH_US` = **2550 µs**
read.

**How often.** A hand scratch is roughly 4–8 back-and-forth cycles per second =
8–16 sign crossings/s. Master drives four heads, so **32–64 redirected reads per
second**, each 2550 µs → **82–163 ms of eMMC occupancy per second, 8–16% duty,
purely from direction changes** — a cost the clamp derivation does not contain
any term for.

**Why a persistent-reverse primitive was reused for a momentary one.** Because
the primitive's contract — "does not move the head, does not flush residency" —
made it look free, and it *is* free at the level it describes. The cost lives one
layer up, in a caller the primitive knows nothing about. The header's claim was
true and the conclusion I drew from it was false.

**And this is precisely what the free-window claim got wrong.** `st_scratch.h`
asserts:

> *"a gesture that oscillates INSIDE the resident ring costs nothing… a head may
> move freely across `ST_SCRATCH_FREE_WINDOW_US` of audio — forward, backward,
> repeatedly — without a single new read."*

Oscillating inside the ring does not avoid reads. It **maximises** them, because
every crossing redirects the producer to an aliasing address. The 53.1 ms free
window is real as a statement about which *sectors a head touches*, and
worthless as a statement about *what the prefetcher will have*.

**Why the test didn't catch it — and this is the general lesson.**
`test_scratch.c:175-240` models residency as *"`ring_slots` sectors resident at
any moment, centred on where the gesture started"*, re-centring on each miss, and
calls `st_stream_sector_ready(&st, s)` unconditionally after every step. That is
an idealised, fully-associative, head-centred cache. **The production ring is
direct-mapped, is filled from a request address the test never computes, and can
say no.** The harness asked "how many distinct sectors does this gesture touch?"
and reported the answer under a function named
`test_oscillating_inside_the_ring_costs_nothing`. It measured the easy question
and the name claimed the hard one.

### 2.6 Master scratch did not manipulate the real transport

**What you heard.** Not a simulated scratch overlay — there is no such code —
but something that sounds like one and is worse: **the four heads decoupled from
each other and from the song clock, so some stems kept advancing at the
transport's forward rate while others were being driven.**

**The trace.**

The v1.3 engine has a load-bearing invariant, stated at `main.c:1796-1807`:

> *"THE TRANSPORT CLOCK IS A HEAD, BUT NOT NECESSARILY STEM 0. The loop window,
> the seam duck, the beat phase and the published song frame belong to the SONG,
> and a reversed head must not drag the song's clock backwards with it. So they
> are all read from this head, **which is always one that is still going
> forward. There is always at least one, because only one track reverses at a
> time** — asserted where the gesture sets it, not assumed here."*

`s_stem_transport` is a **stem index**, maintained by a search
(`main.c:3421-3427`) that runs only on the reverse-request/reload path:

```c
for (j = 0; j < ST_PL_STEMS; j++)
        if (!g_stem_stream[j].reverse) { s_stem_transport = j; break; }
```

**Master scratch violates that invariant categorically.** It drives all four
heads negative at once, and the apply block calls `st_stream_set_reverse()`
**directly, without ever re-running the transport-head search**. So:

- with all four reversed, the search would find nothing anyway and, by its own
  comment, "falls back to leaving the transport where it is";
- `s_stem_transport` keeps pointing at a head that is now travelling backwards;
- everything song-level — `atomic_set(&g_stem_song_frame_pub, …)` at
  `main.c:3155`, the loop window arithmetic, the seam duck, the beat phase —
  is read from a backwards-running head, which the design says must never
  happen.

**Why the rate override did not become the authoritative source transport.** It
never claimed to be. What it wrote was `stem_rate_q16[sk]` — the *resampler's*
rate — plus a `reverse` flag. The **authoritative position** is
`g_stem_stream[sk].song_frame`, advanced by
`st_stream_advance_frames(&g_stem_stream[sk], stem_used[sk])`, and `stem_used[]`
comes back from the renderer. So the gesture drove the head *indirectly, through
the renderer's consumption*, and that indirection is where it broke: a head that
cannot get its sector consumes **zero** source frames and does not move, while
the gesture keeps telling it a rate. The rate was authoritative; the position was
not. Those must be the same thing.

**Why a host test validated behaviour that was false on hardware.** Because
`test_scratch_apply.c` supplies `static st_stream_t g_stem_stream[4]` and
`static uint8_t s_stem_transport` as bare variables. There is no producer, no
mailbox, no ring — and critically, **nothing that can return `UNDERRUN`**. In
that world every head always advances by exactly its count, so "master keeps
four heads locked" is true by construction and the test could not have failed.
The stub did not model the environment; it modelled the assumption.

### 2.7 Vocal / head drift without intentional reverse

Two mechanisms, both real, and the second is permanent.

**(a) Accidental arming.** `st_scratch_drive_from_fader()` converts a delta to
counts-per-second and gates at `ST_SCRATCH_FADER_DEADBAND_CPS = 200`. At the
~125 Hz gesture cadence (8 ms passes), **one** count of jitter = 125 cps →
rejected; **two** counts = 250 cps → **accepted**, producing
`drive = (250−200) × 65536 / 3700 = 885 Q16` — 1.4% of full drive. Musically
nothing. But `scratch_service()` arms on `fdrive[k] != 0`:

```c
for (k = 0; k < ST_PL_STEMS; k++)
        if (fdrive[k] != 0) { c->scr_target = (uint8_t)k; break; }
```

so **two counts of ADC jitter, on any one pass, on any one fader, permanently
claims the gesture for that stem** — and with it sets `function_consumed`
(§2.1). And because main.c polls **all four** faders every pass while FUNCTION is
held and no owner exists (§2.3), four channels are exposed to that two-count
trigger at 125 Hz simultaneously. Across even one second of holding FUNCTION,
that is 500 opportunities. The arming was effectively certain, not unlikely.

There is no threshold on *distance* — only on *speed*. A 2-count blip qualifies
identically to a deliberate 40-count sweep, because the deadband asks "is the
hand moving?" and never "has the hand moved anywhere?".

**Activation / debounce / ramp, in full:**

| Stage | Value | Notes |
|---|---|---|
| first pass of a FUNCTION press | establishes references, returns 0 | ~8 ms |
| fader gate | speed ≥ 200 cps (≈ 2 counts @ 8 ms) | no distance term at all |
| fader arms target | any non-zero drive, one pass | no confirmation, no hysteresis |
| rocker debounce | `ST_CTL_SCRATCH_ROCKER_SETTLE = 2` passes ≈ 16 ms | vs volume's 3 |
| accel ramp | `ST_SCRATCH_ACCEL_MS = 80` ms full traverse | unmeasured |
| decel ramp | `ST_SCRATCH_DECEL_MS = 50` ms | unmeasured |
| publish → audio | one block, `ST_SCR_BLOCK_US = 5333` µs | 256 frames @ 48 kHz |

**(b) Permanent drift, and this is the serious one.** Under master scratch all
four heads get the same rate — but they advance **independently**, each by its own
`stem_used[sk]`, and `st_stream_advance_frames()` on a non-resident head returns
`UNDERRUN`, counts the episode, and **leaves `song_frame` exactly where it is**
(`main.c:4141-4147`) while the other three advance.

**There is no resync anywhere.** That is deliberate and correct for per-track
reverse, where divergence is the feature (`st_scratch.h`: *"the other three are
untouched, keep their own positions, and are never resynced"*). Under master
scratch it is catastrophic: the residency thrash of §2.5 guarantees that some
head loses the race, and every block it starves is a block of permanent
displacement relative to the other three. The vocal you heard off-timeline was
not drifting *during* the gesture — it had been **left behind** by it, and stayed
behind until the song was reloaded.

**Master scratch giving four independent heads the same rate is not the same as
moving one tape.** That is the core design error and the reason a redesign
cannot start from st55.

**Also, a latch-cancelling bug in the same block:** `s_scr_was_rev` is captured
once, from the *grabbed* head — for master, from `g_stem_stream[s_stem_transport]`
— and the coast then forces **all four** heads to that one head's direction. A
stem the player had deliberately reverse-latched is silently flipped forward by
an unrelated master gesture.

### 2.8 Scratch/shuttle slowed to zero and stayed stopped

**Both causes were present.** They are separable and both must be fixed.

**State transitions, as designed:**

| Event | `st_ctl` | apply block |
|---|---|---|
| FUNCTION down, pass 1 | `scr_last_ms` set, references captured, return | `tgt = NONE`, coast (no-op) |
| FUNCTION down, nothing moved | early-out, publishes nothing | `NONE` |
| rocker settles (2 passes) | `scr_target = MASTER`, `rocker_consumed`, `function_consumed` | grab from current signed rate; `set_drive(±FULL)`; `tick` |
| rocker released, FUNCTION still down | `scr_rock → 0`, drive → 0, target **stays latched** | `tick` toward 0 → head slows to a standstill |
| FUNCTION up | `st_ctl_scratch_end()`, target → NONE | `release()` → `coast()` toward the signed transport rate |
| coast arrives | — | `s_scr_owner = NONE`, override drops |

**Why it did not resume +1×:**

- **State-machine contribution.** Drive 0 with the target still latched is a
  *correct* standstill by design — "the hand resting on the record". But the
  target latches for the whole FUNCTION press with no way to disown it, so a
  player who lets go of the rocker while still holding FUNCTION gets a *stopped
  tape* and no way to restart it except releasing FUNCTION. Combined with §2.1
  (FUNCTION release is also the only way to get power-off back) this made the
  device feel locked up.
- **Starvation contribution, and this is why it stayed stopped.** A head in
  `UNDERRUN` recovers only when `st_stream_sector_ready()` is called with the
  sector it needs — which requires the producer to complete a batch at the
  requested address. The scratch was still flipping `reverse` every few tens of
  ms (§2.5), so the request address kept moving and **the producer was
  redirected before it could complete a batch for either direction.** That is a
  livelock, not a slow recovery: the head cannot advance because it has no
  sector, and it has no sector because the address will not hold still.

The audible signature separates them: the *gradual* slowdown is the integrator
plus rising starvation; the *permanent* stop is the livelock.

### 2.9 Isolated scratching crackled, stopped, and restarted from the beginning

**The exact restart path.**

1. A reversed head that retreats past frame 0 parks **on** frame 0 and enters
   `ST_STREAM_START_OF_SONG` (`st_stem_stream.c:214-217`, `:280-286`).
2. `st_stream_advance_frames()` returns `NOT_PLAYING` for that state — the head
   is stuck at 0.
3. The moment the gesture pushes forward, `st_stream_set_reverse(hd, false)`
   lifts `START_OF_SONG → PLAYING` (`st_stem_stream.c:115-118`) **at
   `song_frame == 0`**.
4. The song plays from the top. Under **master**, all four heads take this path
   together — hence "the sound just stops and starts over".

No error handler resets position; no underrun path resets position; the
invariant you state — *"no underrun, residency miss, scratch release or error
handler should ever reset song position"* — was **not violated by an error
path**. It was violated by a *feature* path: a boundary rule written for
persistent reverse, where parking at the start is the specified behaviour
(*"a reversed track that reaches the start stops there"*), reused by a momentary
gesture where it is a destructive seek.

**Why the invariant was absent.** Because "reverse reaching the start parks at
0" was a *specified* behaviour, so no test asserted the complementary property
("nothing else may reposition the head"). The absence of a wrong-reset test is
not an oversight in the reverse work — for reverse it was right. It became wrong
the moment a second, momentary consumer was pointed at the same primitive
without asking whether the boundary semantics transferred. **They did not, and
nothing in the codebase forced that question to be asked.**

The crackle preceding it is §2.3 + §2.5: streamer starvation producing genuine
dropouts, not a decode fault.

### 2.10 Sensitivity and latency were wrong

**Every source of delay between finger and audible movement, for a master
rocker scratch:**

| Stage | Latency |
|---|---|
| control pass quantisation | up to 8 ms |
| first pass of the FUNCTION press (establishes references, returns 0) | 8 ms |
| rocker debounce, `ST_CTL_SCRATCH_ROCKER_SETTLE = 2` | 16 ms |
| `stem_ctl_apply()` → audio thread, one block | 5.3 ms |
| decel +1.0× → 0 at `DECEL_MS = 50` ms per 2.0× traverse | 25 ms |
| accel 0 → −1.0× at `ACCEL_MS = 80` ms per 2.0× traverse | 40 ms |
| **total to a full reversal** | **≈ 100 ms** |
| **total to any audible movement** | **≈ 30–40 ms** |

**Why those values were chosen.** `ACCEL_MS = 80` was chosen so that *"a ~100 ms
press produces a real excursion rather than a wobble"* — i.e. tuned so a short
press *does something*, which is a different requirement from *does something
immediately*, and I substituted the first for the second. `DECEL_MS = 50` was
chosen by analogy ("the tape under a resting hand stops sooner than it starts").
The rocker's 2-pass settle was a genuine compromise against documented AIN1
noise, and it is the only one of the three with a stated reason grounded in this
hardware — but it was still guessed, not measured.

**Against your stated requirement** — *"scratch response needs to begin
essentially immediately, not after an 80 ms ramp"* — this is a straightforward
miss. The requirement was on the table before the code was written. I chose feel
parameters from analogy instead of from the requirement, and shipped them
unmeasured.

---

## 3. Failure-prevention architecture

### 3.0 The escape-hatch rule — above everything below

Everything in §3.1–§3.9 is engineering discipline. This one is not negotiable
against any of it, and it is stated first because the rest of the document is
subordinate to it.

> **The device must always be able to escape bad firmware immediately, without
> waiting for battery depletion. This holds for the dedicated development unit
> exactly as it holds for a production one.**

Four consequences, in force:

**E1 — "Test device" is not a mitigation.** A development SP-1 is the unit that
runs the *most* experimental firmware, so it needs escape more than any other
unit, not less. Nothing in a risk assessment may discount a stranding failure on
the grounds that the affected device was a test device. In this incident that
framing would have hidden the entire defect: the build did not merely misbehave,
it removed the property that makes experimenting safe at all.

**E2 — Recovery outranks every feature state.** Power-off and bootloader entry
sit above the whole musical layer in priority. There is no feature state,
gesture, mode, chord, latch, overlay or error condition from which they are
unavailable. "Higher priority" here means *structurally unreachable by feature
code*, not "checked first".

**E3 — No feature may consume, mask, delay, reset or otherwise influence either
path.** Including indirectly: a feature must not be able to affect the *timer*,
the *state* the timer lives in, or the *branch* the timer is evaluated in.
§2.1's failure was indirect — nothing "blocked" `power_off()`; a flag reset
`press_start` and the timer stopped existing.

**E4 — At least one escape route must be outside the application entirely.**
Today that is the bootloader combo: SP-1 **off**, hold **Track 1 + Track 4**,
insert USB → UF2 mode (one track light). It lives in the bootloader, so no
application firmware can intercept it. It must never be traded away, and if a
future change would make it conditional on anything the application does, that
change does not ship.

**Classification rule.** A build that touches **power, FUNCTION dispatch,
bootloader entry, watchdog behaviour, transport recovery, or input arbitration**
is **automatically high-risk**. High-risk builds do not produce a flashable
artifact until the dedicated safety tests in §4 have run and passed. This is a
property of the *diff*, not of anyone's judgement about how risky the change
feels — the st55 diff touched four of those six, and no one classified it as
anything.

st55 violated E1, E2 and E3. E4 is the only reason this incident ended in a
revert rather than a dead unit.

Every invariant below is stated so it can be **mechanically checked**. An
invariant without a named enforcement mechanism is a wish, and this document's
whole thesis is that the previous round had plenty of wishes.

### 3.1 Power and recovery

| # | Invariant | Enforcement |
|---|---|---|
| P1 | The shutdown timer is not inside any feature-gated branch. `press_start`/`held` are maintained by a dedicated `power_hold_service()` called **unconditionally**, before any dispatcher, taking only the raw FUNCTION GPIO. | New host test `test_power_hold.c`: 2500 ms of FUNCTION down fires shutdown **for every reachable combination of consumed-flags** — exhaustive over the flag set, not a sampled subset. |
| P2 | No feature may consume, mask, delay or reset the long-hold. `function_consumed` is renamed `function_musically_consumed` and is **structurally incapable** of reaching the power path — the power service does not take it as a parameter. | Wiring gate: the power service's call site must have **no** `g_stem_ctl_out` reference in its condition. Replaces the current gate that pins the opposite. |
| P3 | The existing gate at `stem_playback_wiring_check.py:453` is **deleted and inverted**: the line it requires is the bug. | The gate that replaces it asserts the *absence* of any `function_consumed` term guarding `power_off()`'s enclosing branch. |
| P4 | The bootloader route stays reachable independently of firmware: SP-1 **off**, hold **Track 1 + Track 4**, insert USB → UF2 mode (one track light). It lives in the bootloader, not this firmware, and nothing here can intercept it. (E4.) | Documented in the release notes of every build; already true, now stated so it is never traded away. |
| P5 | Musical consumption and safety suppression are **different types**. Two fields, never one boolean. | Compile-time: the power service's signature cannot accept the musical type. |
| P6 | **If an application-level recovery or bootloader-entry path exists, it is regression-tested like power-off.** Today there is none in the application — the only route is P4's bootloader combo, which this firmware cannot reach or influence. If one is ever added, P1/P2/P3's treatment applies to it identically and unconditionally. | The absence is asserted: a gate fails if any application code writes the bootloader-entry register or double-reset marker, so a route cannot appear untested. |
| P7 | **Any new FUNCTION handling ships with an explicit test proving a 2.5 s power hold still works from every possible feature state.** Not a sampled subset: the test enumerates the feature-state space (loop armed/active/latched, FX overlay held, reverse latched per stem, solo/mute combinations, transport playing/stopped, any future gesture state) and asserts shutdown from each. | `test_power_hold.c`, and a checklist item that fails review if a diff touches FUNCTION dispatch without extending its enumeration. |

### 3.2 Input ownership

| # | Invariant | Enforcement |
|---|---|---|
| I1 | A gesture has exactly one owner, named in one place. Arbitration is a single explicit table, not a set of `!consumed` guards scattered across handlers. | `test_ctl.c`: for every (control × owner) pair, exactly one consumer sees the event. |
| I2 | Once scratch owns the rocker or a fader, that event **cannot** reach volume, tempo, mute, solo, reverse, FX or fader-gain — including the inherited Looper decode. | The inherited decode must be **unreachable** while a stem song is selected and a gesture is live, structurally (an early `continue` that no feature flag controls), not by per-handler masks. |
| I3 | **FUNCTION alone has zero musical side effects and zero cost.** No extra sampling, no state change, no consumption, until an explicit arm. | `test_ctl.c`: FUNCTION held for 1000 simulated passes with all inputs static produces a bit-identical output struct every pass. Plus an ADC-call-count assertion (§3.5). |
| I4 | Solo/mute/gain writes are gated on the ladder decode **and** on no live gesture. | Wiring gate on `stem_ctl_apply()`. |

### 3.3 Real transport only

| # | Invariant | Enforcement |
|---|---|---|
| T1 | Scratch manipulates the **authoritative head position and signed rate**. There is exactly one audible position per stem and the gesture writes it directly, not via renderer consumption. | The apply path must call the position API; a wiring gate asserts the renderer's `stem_used[]` is not the only thing that moves a head during a gesture. |
| T2 | No simulated overlay, no parallel forward transport. | Full-playback gate hash unchanged when no gesture is live; a new gate asserts position **equality** between what the gesture commanded and what the head reports. |
| T3 | **Master scratch moves one shared head, not four heads given the same number.** Phase lock is structural — the four stems share a position and can only diverge when a per-stem gesture explicitly diverges them. | `test_scratch_apply.c` with a starvation-capable stub (§3.8): after any sequence of starvation events during a master gesture, all four `song_frame` values are **equal**. |
| T4 | The song-clock invariant is enforced, not assumed: if no head is forward, that is an error, not a fallback. | `_Static_assert` where possible; a runtime assertion plus a diagnostic counter where not. |

### 3.4 Residency

| # | Invariant | Enforcement |
|---|---|---|
| R1 | Rapid signed-rate changes must not invalidate the ring. Direction is a property of head traversal, **not** a reason to change the prefetch request address. | A **bidirectional** read-ahead: the request covers sectors on both sides of the head so a sign flip needs no new address. Requires either a larger G or a smaller R — see §5. |
| R2 | Persistent reverse and momentary scratch may share a primitive only if the primitive's **boundary semantics** transfer. They do not (§2.9), so momentary scratch gets a *clamping* boundary, not a *parking* one. | `test_scratch.c`: a gesture driven past frame 0 and back returns to its starting frame ± the commanded displacement — never to 0. |
| R3 | The free-window claim must be measured **through the production prefetcher**, not through a model of it. | The oscillation harness is rewritten to drive the real request-address computation and the real direct-mapped slot arithmetic, and to report **evictions**, not distinct sectors. It must be capable of reproducing the 7-sectors-over-6-slots aliasing before it is trusted to deny it. |
| R4 | Slot aliasing between the two directions' read-ahead regions is a **compile-time** error. | `_Static_assert` that forward-ahead ∪ head ∪ reverse-ahead ≤ G distinct slots. With R = 3 that requires **G ≥ 7**. |

### 3.5 Streamer safety

| # | Invariant | Enforcement |
|---|---|---|
| S1 | No blocking ADC or diagnostic work is added to the hot loop without a **measured** budget first. | A new CI gate counts `ladder_read()` / `adc_read_dt()` call sites reachable per control pass and fails above a pinned ceiling. The ceiling starts at the baseline's count. |
| S2 | Scratch input sampling fits inside the already-proven v1.3 streaming budget — it does not get its own. | The clamp derivation gains a **control-thread term**. A clamp that bounds only the eMMC while the control thread steals the CPU that serves it is not a bound. |
| S3 | Normal playback and unaffected stems never starve because a control is being sampled. | Physical test, on hardware, before any flash is offered: sustained gesture at maximum rate with `g_starve_cnt[]` and `g_stem_underrun_count` read out and required to be **zero**. |
| S4 | The measured hardware calibration in `ladder_read()`'s comment (24 conv/pass = starves, 6 = safe) is promoted from a comment to a **checked constant**. | The S1 ceiling is that constant. |

### 3.6 Head integrity

| # | Invariant | Enforcement |
|---|---|---|
| H1 | Untouched stems cannot change head position, direction, gain, mute or solo. | Snapshot-and-compare in the apply test, across starvation events. |
| H2 | An isolated scratch affects exactly one stem. | Same test, all four stems as the target in turn. |
| H3 | Master scratch moves all four with **zero** relative drift (T3). | Same test. |
| H4 | Scratch exit repositions nothing. Release semantics are: the head stays where the gesture left it; direction returns to the player's latch; rate coasts to the transport's. | Test asserts `song_frame` at release == `song_frame` after coast completes, for every combination of latched reverse and pitch. |
| H5 | A master gesture may not overwrite another stem's deliberate reverse latch. | Fixes the `s_scr_was_rev` bug (§2.7); tested. |

### 3.7 Failure behaviour

| # | Invariant | Enforcement |
|---|---|---|
| F1 | An underrun never restarts the song, never moves the head, never changes gain/mute/solo. | Starvation-capable stub test asserting all four. |
| F2 | An error never silently changes transport position. Any position change has exactly one commanding call site. | Wiring gate: `st_stream_seek()` call sites are enumerated and pinned; a new one fails CI until justified. |
| F3 | Any starvation fallback is **separately counted and obvious** in development builds. | `g_starve_cnt[]` / `g_stem_underrun_count` already exist; add a visible LED signature in the non-shipping build, and print the counters in the release-identity CI step. |
| F4 | No fade, filter, limiter or underrun concealment is used to hide starvation. (Your standing rule; restated because it is load-bearing for F3.) | Forbidden-substring gate. |

### 3.8 Gesture activation

| # | Invariant | Enforcement |
|---|---|---|
| G1 | **No permanent head displacement from an accidental brush.** | G2 + G3. |
| G2 | Isolated scratch requires FUNCTION **already held** *and* a **cumulative displacement** from the captured start position exceeding a measured threshold — a *distance* gate, not only a *speed* gate. | `test_ctl.c` replays recorded resting-hand fader traces (§5) and asserts zero arming across the whole capture. |
| G3 | Arming thresholds are derived from **measured** fader-rail noise, the way the AIN0 bands were (`firmware/stemtape_player/docs/ladder-measured.json`). Until that capture exists, isolated fader scratch **does not ship**. | Build-time: the isolated-fader path is `#if`-gated off in the absence of the measurement file. |
| G4 | The fader's ordinary gain value is unchanged when scratch ends — the stem's volume is exactly what the fader position says. | Test. |
| G5 | A gesture can be **disowned** without releasing FUNCTION (fixes §2.8's latch trap). | Test: rocker released → target clears → transport resumes. |

### 3.9 Responsiveness

| # | Invariant | Enforcement |
|---|---|---|
| N1 | Scratch begins immediately enough to feel physically connected. Target: **< 10 ms** finger-to-audible, versus st55's ~30–40 ms to any movement and ~100 ms to a full reversal. | Latency budget asserted at compile time from the constants (debounce passes × pass period + block period + ramp-to-first-audible), so a change to any constant that breaks the budget fails the build. |
| N2 | **The 80 ms ramp does not return** unless hardware testing shows it is musically wanted. Default to the shortest ramp that is click-free, measured. | N1's budget. |
| N3 | Input filtering is tuned for noise rejection from **measured** noise, not guessed cadence arithmetic. | §5's captures. |

---

## 4. Process changes

Agreed and in force from now:

1. **Restore and tag the last known-good build.** Restored to **st54** — the
   build you last flashed and confirmed working. The tree is `d9aedfc` plus this
   document; build tag `st54`; the firmware image is byte-for-byte the one CI
   produced for `d9aedfc`:

   | | |
   |---|---|
   | tree | `d9aedfc9ba1c2a33c0e4c8c96a15b73a98535003` |
   | build tag | `st54` |
   | bin | 115,148 bytes |
   | **sha256** | **`44ed7885b9e6fff2cf0f1d7b6ec418d64e0d7e2d38cd29a28a6de5a68d77ec9c`** |
   | FLASH / RAM | 115,148 B (12.61%) / 203,486 B used, 58,658 B free of 256 KB |

   For contrast, so the two are never confused again: `st53` (`bd8114b`) builds
   to `cb9d4a73731877ee0c7146be86a94d3e048253458d77c5d7bbd4fc3fd84eb713` at the
   same size. **If a flashed image hashes to `cb9d4a73…`, it is st53 and the
   Track LEDs will read 256× low.**

   The tag is **not** pushed: `st54-known-good` was created locally but
   `git push origin st54-known-good` is refused with HTTP 403. This session's
   credentials can write `refs/heads/claude/*` and cannot create tags, and that
   is not something to route around. From a checkout with normal push rights:

   ```
   git tag -a st54-known-good d9aedfc -m "st54: last build confirmed working on hardware"
   git push origin st54-known-good
   ```

   Until then the restore point is the SHA, which is an ancestor of this branch
   and named here and in the restore commit — so losing the tag does not lose
   the restore point.

2. **Preconditions for a flashable artifact.** A build is not offered for
   flashing — and a high-risk build (§3.0's classification rule) does not
   produce an artifact at all — until every one of these has run and passed:

   | | Precondition | Enforced by |
   |---|---|---|
   | a | **Normal power-off is regression-tested.** 2.5 s FUNCTION hold fires shutdown. | P1, `test_power_hold.c` |
   | b | **Application-level recovery / bootloader entry is regression-tested if one exists.** If none exists, its absence is asserted so one cannot appear untested. | P6 |
   | c | **Musical gesture handling is provably unable to consume either path.** Not "checked first" — structurally unable, by type and by call signature. | P2, P5 |
   | d | **Recovery has higher priority than every feature state.** | E2, P1 |
   | e | **Any new FUNCTION handling has an explicit test proving a 2.5 s hold still works from every possible feature state.** | P7 |
   | f | **Any build touching power, FUNCTION dispatch, bootloader entry, watchdog behaviour, transport recovery, or input arbitration is automatically high-risk** and requires the dedicated safety tests above before an artifact is produced. Classification is by diff, not by judgement. | §3.0 |

   st55 would have been stopped by (a), (c), (d) and (e) independently, and
   would have been classified high-risk by (f) on four of the six triggers.

3. **Replacement scratch work stays on its own branch**, off
   `claude/stemtape-m0-safety-audit-1vg9pq`, and does not merge into the
   baseline branch until it has been flashed and accepted on hardware.
4. **Automated regression tests before any new scratch code**, covering:
   power-off reachability (P1/P2), FUNCTION arbitration (I1–I4), gain/solo/mute
   isolation (H1, F1), head synchronisation (T3/H3), no transport reset
   (F1/F2/R2), residency survival across rapid direction changes (R1/R3/R4).
   **These land first, on the baseline, and pass, before a line of scratch code
   is written.** Several of them will fail against st54 as written — that is the
   point, and those are baseline bugs to fix or explicitly accept.
5. **A deterministic recovery path scratch cannot intercept** — P1–P4. The
   bootloader combo (P4) is the hardware backstop; P1's unconditional
   `power_hold_service()` is the firmware one.
6. **Staged build-up, one flashable increment at a time:**

   | Stage | Scope | Must remain true |
   |---|---|---|
   | 0 | Regression tests only, no feature | Everything st54 does |
   | 1 | Input ownership only — arbitration, **no transport movement** | + E1–E4, P1–P7, I1–I4 |
   | 2 | Real **master** head movement, low signed rates, no sign changes | + T1–T4, H1–H4 |
   | 3 | Repeated sign changes without residency invalidation | + R1–R4, S1–S4 |
   | 4 | Release / coast behaviour | + H4, F1–F4 |
   | 5 | Isolated one-stem movement — **only after** the fader-noise capture | + G1–G5 |
   | 6 | Acceleration and feel tuning | + N1–N3 |

7. **At every stage, everything previously working stays working** — playback,
   pitch, FX, loop, slow mode, reverse, power. Proven by the existing gates
   (including the `0x2a737e00` playback hash) plus the new ones, not by
   inspection.
8. **No flash request without a written delta.** Every request to flash states:
   what changed from the previous known-good build, which regressions are
   mechanically prevented and by which named test, and — new — **which
   properties are still unproven and what the worst case is if they are wrong.**
   st55's failure was not that I was unaware of the risks; it was that I never
   wrote down what I had not proven.

**One more, added from this analysis and not on your list:**

9. **A CI gate that pins a source line must state which property it protects, and
   the complementary property must also be gated.** The gate at
   `stem_playback_wiring_check.py:453` froze a construct in place to protect the
   loop latch, and in doing so made the power-off bug un-fixable without turning
   CI red. Any gate that pins an `if` condition is pinning *both* branches, and
   both need a stated owner.

---

## 5. Measurements this device still owes

Nothing above closes without these. Listed so they are not quietly skipped again.

| # | Measurement | Blocks |
|---|---|---|
| M1 | `adc_read_dt()` wall time on this build, per channel, on the main thread | S1's ceiling; any honest CPU budget |
| M2 | AIN0 raw trace under the baseline schedule vs. a 4-fader schedule, FUNCTION held, no fingers | Confirms or refutes §2.4's coupling attribution |
| M3 | Resting-hand fader noise on AIN3/6/2/7 — the equivalent of `firmware/stemtape_player/docs/ladder-measured.json` | G2, G3, N3; **isolated fader scratch does not ship without it** |
| M4 | Streamer throughput vs. control-pass ADC count, swept | S1, S2, S4 |
| M5 | Shortest click-free accel ramp, by ear, on hardware | N1, N2 |
| M6 | The v1.3 upload path against real hardware — **still never run** | Unrelated to scratch, still outstanding, still the largest untested surface in the project |

---

## 6. Summary table

| Failed implementation choice | Hardware symptom | Root cause | Replacement design | Regression test |
|---|---|---|---|---|
| `scratch_service()` asserts `function_consumed` continuously, and the 2.5 s timer lives inside the branch that flag guards | **Device could not be switched off; stayed on until battery drain** | One boolean served both "don't double-interpret this press" and "don't run the shutdown timer"; `press_start = -1` on the fall-through destroyed the timer outright | Unconditional `power_hold_service()` taking only the raw GPIO; musical vs. safety consumption are different types; power path cannot reference gesture state | `test_power_hold.c` — shutdown fires at 2500 ms for **every** reachable consumed-flag combination (P1, P2) |
| CI gate pins `if (pwr_pressed() && !…function_consumed)` as required source text | Bug was frozen in place; fixing it would have turned CI red | Gate written to protect the loop latch, complementary property never gated | Delete and invert: assert the **absence** of any gesture term guarding `power_off()`'s branch | The inverted gate itself (P3) |
| Poll all four faders every pass while FUNCTION is held, before an owner exists | Crackling, transport dragging to a halt | 14–16 conversions/pass vs the baseline's 4 during a hold — past halfway to the 24/pass already measured to starve the card; no CPU budget gate exists | Explicit arm step; sample only after arming; ADC call-count ceiling pinned at the baseline's | ADC-call-count CI gate (S1, S4) + zero-starvation hardware test (S3) |
| Added converter traffic to the shared `BTN_COM` rail to read controls on that rail | **Phantom Vocal solo** while merely holding FUNCTION | Systematic periodic rail sag; T1's band (180–230) is the one nearest idle (≤110); `st_ladder`'s debounce rejects uncorrelated noise, not correlated bias | No added sampling in the un-armed state; arm from a control that is not on the contested rail; thresholds from measured rail noise | M2 capture + `test_ctl.c` replay of recorded traces (G2, G3, I3) |
| Prefetch address flips `needed+1` ↔ `needed−R` on every sign change | Dropouts; heads freezing and never recovering | Direct-mapped ring, R=3/G=6 → forward and reverse read-ahead span **7 sectors over 6 slots** and provably alias; every flip evicts and costs 2550 µs; 32–64/s under master | Bidirectional read-ahead — one request covering both sides so a flip needs no new address; G ≥ 7 | `_Static_assert` on slot-set size (R4) + oscillation harness rewritten onto the **real** request-address arithmetic (R3) |
| Free-window claim measured through an idealised head-centred cache model | "Oscillating is free" was false in exactly the case scratching lives in | Test modelled residency instead of using it; asserted `sector_ready()` unconditionally — the fact under test | Harness must reproduce the aliasing before it is trusted to deny it; report evictions, not distinct sectors | R3 |
| Master scratch = four independent heads given the same rate | **Vocal permanently off-timeline from the other stems** | `UNDERRUN` freezes one head while three advance; there is no resync anywhere, by design | Master moves **one shared position**; per-stem divergence only from an explicit per-stem gesture | Apply test on a **starvation-capable** stub: all four `song_frame` equal after any starvation sequence (T3, H3) |
| Gesture drives position *indirectly*, through renderer consumption | "Simulated scratch over normally-advancing audio" | Rate was authoritative, position was not; a starved head consumes 0 frames and does not move while the gesture keeps commanding a rate | Gesture writes the authoritative position directly; rate and position are one thing | Commanded-vs-actual position equality gate (T1, T2) |
| `st_stream_set_reverse()` reused for momentary sign changes | Song **restarted from the beginning** | Backward head parks at frame 0 (`START_OF_SONG`); the next forward flip lifts it to `PLAYING` **at 0**. Correct for persistent reverse, destructive for a momentary gesture | Momentary scratch gets a **clamping** boundary, not a parking one; boundary semantics are part of the primitive's contract | Drive past frame 0 and back → returns to start ± commanded displacement, never 0 (R2, F1) |
| `s_scr_was_rev` captured from the transport head, applied to all four on coast | A deliberately reverse-latched stem silently flipped forward | Single static shared across a four-head gesture | Per-stem captured direction; a master gesture may not write another stem's latch | H5 |
| Fader arms on **speed** (≥200 cps ≈ 2 counts) with no distance term, one pass, no confirmation | Vocal displaced by an accidental brush; gesture armed by noise alone | Deadband asks "is the hand moving?", never "has it moved anywhere?"; 4 channels × 125 Hz made accidental arming near-certain | Cumulative-displacement gate from **measured** noise; isolated fader scratch build-gated off until M3 exists | Recorded resting-hand trace → zero arming (G2, G3) |
| Target latches for the whole FUNCTION press, undisownable | Tape stopped and stayed stopped with no way back short of releasing FUNCTION | No disarm path; compounded by the power-off trap making FUNCTION release feel unsafe | Releasing the control disowns the gesture; FUNCTION release is not the only exit | G5 |
| 2-pass debounce + 80/50 ms ramps, unmeasured | "Not sensitive enough" | ~30–40 ms to any movement, ~100 ms to full reversal; ACCEL tuned so a press *does something*, not so it does something *immediately* | Compile-time latency budget < 10 ms finger-to-audible; ramps measured, not analogised | Static latency-budget assertion (N1, N2) |

---

## 7. What this cost, stated plainly

A performance feature was allowed to sit between the user and the power button.
Everything else in this document — the starvation, the phantom solo, the drifting
vocal, the restarts — is ordinary engineering failure of the kind tests are
supposed to catch, and the tests were weak in ways §1 and §2 now name precisely.

The power-off failure is a different category. It was reachable because a single
boolean was doing a musical job and a safety job at once, because the shutdown
timer happened to live inside the branch that boolean guarded, and because the
one CI gate that touched that line was pinning the construct rather than the
property. None of those three is a subtle bug. All three were visible in the
source I was editing.

And the unit it happened to was the dedicated test device — which is the point,
not the mitigation. A development SP-1 is the one that runs the most
experimental firmware, so it is the one that most needs to be able to escape it.
A build that removes that ability has not merely misbehaved; it has taken away
the thing that made experimenting on that unit safe in the first place. §3.0 is
in the document because of this incident, and it outranks everything else in it.

**st54** (`d9aedfc`, bin `44ed7885…ec9c`) is the restore point (§4.1). The staged
plan in §4 does not begin until §3's tests exist and pass.
