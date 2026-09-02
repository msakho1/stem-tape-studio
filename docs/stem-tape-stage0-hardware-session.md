# Stage 0 hardware acceptance — the session plan

**Candidate:** `st61` · commit `a567d49c384260d179d29ca2dbe0077c1c62ad31` ·
sha256 `a9f37df90a6a251d8e7ede5796e9c23703c3a3b49ebc963383f38d88e18918ce` ·
116,432 B · FLASH 12.74% · RAM 203,486 / 58,658 B free.

Verify the hash before flashing. If a flashed image hashes to
`44ed7885…ec9c` it is st54; to `cdd89a3d…12d2` it is st60 (no transfer-mode
escape hatch); to `719f8500…fae25` it is st59 (no idle shutdown).

---

## 0. One blocker to decide before the session

**M3 cannot be measured with st61.** No raw fader value is printed anywhere in
the production image — the diag line carries `g_master_vol_q8` (the AIN1 master
volume) and the per-track `vol_q8` is quantised to 256 steps ≈ 14.5 raw counts,
which is the same order as the 16-count threshold being measured. Backing the
noise floor out of it is not possible.

Two options, and this is your call:

* **A — a separate capture image**, `st61-FADERCAL`, following the precedent
  already in the tree: CI builds an "AIN1 CALIBRATION IMAGE (separate build,
  never the shipped one)" the same way. It would print raw AIN3/6/2/7 at a
  fixed cadence and nothing else. **It is a firmware change** — to a
  non-shipped image only, never to st61 — so it needs your approval, and it is
  the reason the session below has two flashes.
* **B — defer M3**, keep `ST_PWR_FADER_MOVE_COUNTS = 16` as an estimate, and
  accept both named risks: resting noise above 16 counts means the device never
  sleeps *and* the manual hold is reset by noise; a threshold too high means
  real movement goes undetected. Stage 0 then closes with M3 outstanding.

Everything else in this document runs on st61 unmodified.

---

## 1. Ordering, and why

**Bootloader recovery is first, before anything is flashed.** If it does not
work you must not flash — you would be putting an unproven image on a device
with no proven way back. Nothing else depends on firmware either.

After that the order minimises power cycles and puts the ~40 minutes of
five-minute waits last, where they can run while you do something else.

| # | Block | Flash | Serial | Wall time |
|---|---|---|---|---|
| 1 | Bootloader recovery | — | — | 5 min |
| 2 | M3 fader noise *(option A only)* | FADERCAL | **yes** | 25 min |
| 3 | Wake + release-to-rearm | **st61** | no | 15 min |
| 4 | Manual shutdown + interruptions | — | no | 20 min |
| 5 | Transfer-mode shutdown | — | no | 15 min |
| 6 | Shutdown latency | — | no | 10 min |
| 7 | Musical regression vs st54 | — | no | 20 min |
| 8 | The five-minute battery | — | no | ~45 min |
| 9 | Overnight idle/noise | — | no | overnight |

**Two flashes total** (one if you take option B).

### On measuring milliseconds by hand

You cannot hand-time 1.999 vs 2.000 s, and you should not try. Those exact
boundaries are host-proven at 1 ms granularity; hardware's job is to confirm
the mechanism works and the threshold is *approximately* right. Where a number
matters, film it: a phone at 240 fps gives ~4 ms resolution and a timestamped
frame for the record. Anywhere below is written as a behavioural pass/fail, not
a stopwatch reading.

---

## 2. Block 1 — bootloader recovery (before flashing)

| # | Action | Pass |
|---|---|---|
| R-1 | SP-1 **off**. Hold Track 1 + Track 4. Insert USB. | **one** track light; a UF2 drive mounts |
| R-2 | Eject, unplug, repeat once | same, repeatably |

**If R-1 fails, stop.** Do not flash. The bootloader's source is not in this
repository — this is the only evidence that will ever exist for it here, and
it is the recovery path for everything that follows.

Note the cue: the bootloader lights **one** track LED. The removed in-firmware
`enter_dfu()` lit **all four**. Seeing four means you are running old firmware,
not the bootloader.

---

## 3. Block 2 — M3 fader noise *(option A only)*

Capture raw AIN3 / AIN6 / AIN2 / AIN7 for ~30 s per condition, hands off the
faders except where stated. Record min, max, and max sample-to-sample delta.

| # | Condition | Record |
|---|---|---|
| M3-1 | resting, nothing running | floor |
| M3-2 | during normal playback | |
| M3-3 | during heavy eMMC activity (start a large upload) | |
| M3-4 | USB attached and enumerated, host idle | |
| M3-5 | FX engaged over playback | |
| M3-6 | pitch/varispeed active | |
| M3-7 | reverse active | |
| M3-8 | **very slow** intentional movement (one end to the other over ~10 s) | smallest real delta |
| M3-9 | ordinary performance movement | |
| M3-10 | fast movement | |

**Choosing the threshold.** It must sit above `max(M3-1…M3-7)` sample-to-sample
delta with documented margin, and below the smallest delta seen in M3-8. If
those two overlap there is no valid threshold and the detector needs a
different discriminator (e.g. N consecutive same-sign deltas) — report that
rather than picking a number in the overlap.

---

## 4. Block 3 — wake and release-to-rearm (st61 flashed)

Do the **charge-standby** column first (USB attached), then repeat the whole
column on **battery** with USB unplugged. Both must behave identically.

| # | Action | Pass |
|---|---|---|
| ON-1 | tap FUNCTION | stays off |
| ON-2 | hold ~1 s | stays off |
| ON-3 | hold ~0.6 s *(the old threshold)* | **stays off** |
| ON-5 | hold ~2 s cleanly | turns on |
| ON-6 | release at ~1.5 s, re-press | needs a fresh full hold |
| ON-7 | at ~1.9 s press a Track | resets |
| ON-9 | 20 short taps | never turns on |
| **ON-11** | wake, then **keep holding** 5 s / 10 s / 30 s | **stays on** |
| **ON-12** | during ON-11, watch the display from the moment of wake | **boots normally under the held finger** — st54 stalled dark here; this is the one deliberate behavioural change and the thing most likely to surprise |
| **ON-13** | release after ON-11, then hold ~5 s | powers off |
| **ON-14** | tap several times, then hold 2 s and keep holding 10 s | wakes once, does **not** then power off |
| ON-15 | force a watchdog/fault reboot, don't touch anything, then hold 5 s | powers off |

Record the **measured** wake time for ON-5 on both paths.

---

## 5. Block 4 — manual shutdown and interruptions

| # | Action | Pass |
|---|---|---|
| OFF-1 | FUNCTION only, ~4.5 s, release | stays on |
| OFF-2 | FUNCTION only, ~5 s | powers off — **record measured time** |
| OFF-3 | release at ~3 s, re-press | full fresh hold needed |
| OFF-4…7 | at ~4.5 s press PLAY / each Track / rocker both ways / VOL ± | timer resets each time |
| OFF-8 | move **each** of the four faders during a hold | resets — all four |
| OFF-9 | after each interruption, stop and keep holding | full fresh hold |
| **OFF-10** | move a fader **before** pressing FUNCTION, then hold | **~5 s, not ~25 s** — this is the 24.7 s defect |
| OFF-11 | hold every FUNCTION chord >5 s while the second control stays active | **never** powers off |

OFF-11 chords: FUNCTION + PLAY · + each Track · + Track double-tap (reverse) ·
+ rocker FWD · + rocker RWD · + VOL ± · + the FX entry chord · bank jump ·
grid clear · slow-mode toggle · loop latch.

---

## 6. Block 5 — transfer-mode shutdown

The new behaviour, and the one with storage at stake.

| # | Action | Pass |
|---|---|---|
| X-1 | start a large upload; while it runs, hold FUNCTION ~4.5 s | stays on, upload continues |
| X-2 | **during the same upload, hold FUNCTION ~5 s** | **powers off** |
| X-3 | power back on | **the previous song is still there and plays correctly** |
| X-4 | repeat X-2 at three different points: early, mid, and immediately after the companion reports the final flush | previous song intact in the first two; the third may legitimately be the *new* song if the commit block landed — either outcome is a pass, a corrupt or half-written song is not |
| X-5 | re-run the interrupted upload to completion | succeeds normally |
| X-6 | start an upload, leave the host idle mid-transfer, wait ~20 s | transfer mode self-clears; the device behaves normally |

X-4 is the direct hardware test of the A/B claim: only a fully-written index
block carrying the magic and a higher generation can promote new media.

---

## 7. Block 6 — shutdown latency, two separate numbers

| | Measure | Expected |
|---|---|---|
| **acceptance** | hold completes → LEDs change | ≤ one control pass; should look instant |
| **dark** | hold completes → display fully dark, device off | typically tens of ms; **worst case ~13.8 s** |

Measure both at 240 fps. Then force the worst case: start a large upload, let
the eMMC cache fill, hold 5 s, and time it again. **The acceptance number must
stay instant even when the dark number is long** — that is the whole point of
the st61 ordering change.

---

## 8. Block 7 — musical regression against st54

No power involvement. Confirm unchanged: clean playback · loop · reverse ·
pitch/semitone · slow mode · FX · mute/solo · Track LEDs and meters ·
bank/grid. Listen for anything the three extra ADC reads per transfer pass
might have cost — the read count in ordinary playback is unchanged, but this
is the first hardware run since the reads moved.

---

## 9. Block 8 — the five-minute battery

Each row is ≥6 minutes. Start one, walk away, come back.

| # | Condition | Pass |
|---|---|---|
| ID-1 | stopped, untouched | **powers off at ~5:00 — record the measured time** |
| ID-2 | a song playing continuously past 5:00 | stays on |
| ID-3 | a song **longer than 7 minutes**, played end to end | plays to the end |
| ID-4 | loop | stays on |
| ID-5 | reverse | stays on |
| ID-6 | slow | stays on |
| ID-7 | pitched/varispeed | stays on |
| ID-8 | FX over live playback | stays on |
| ID-9 | stop playback, then wait | the interval starts **at the stop** (after the spin-down), not at the last button press |
| ID-10 | at ~4:50 press PLAY / a Track / the rocker / VOL / **each fader** | resets; another full 5:00 needed |
| ID-16 | stopped, then hold FUNCTION 5 s | manual shutdown still fires on time |

## 10. Block 9 — overnight

| # | Condition | Pass |
|---|---|---|
| ID-15 | leave it on, stopped, untouched, on a bench overnight | powers off once at ~5:00 and **stays** off |

ID-15 is what M3 is really about. If the resting noise floor exceeds the fader
threshold, the idle timer is reset by noise for ever and the device never
sleeps — the same battery-drain failure this whole stage exists to prevent,
arriving by a different road. If ID-15 fails, M3 is no longer optional.

---

## 11. What to record

For each block: pass/fail per row, the **measured** times where the table asks
for one, and anything surprising even if it passed. The rows most likely to
surprise, in order: **ON-12** (booting under a held button was impossible
before), **X-4** (the first hardware test of the A/B commit claim), **ID-9**
(the spin-down boundary), and **ID-15**.

Stage 0 is complete when every row above has a result and M3 is either measured
or explicitly deferred with the risk accepted. Not before.
