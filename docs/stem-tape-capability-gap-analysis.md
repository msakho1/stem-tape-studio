# Stem Tape — Complete Capability Gap Analysis

**Date:** 2026-08-20
**Sources:** the capability workbook (9 sheets, 381 rows) read in full, against the
actual state of `firmware/stemtape_player/` at commit `e8e189b` and the companion
at `origin/main`.
**Method:** every claim below is either (a) a row quoted from the workbook, or
(b) a fact I verified by reading the source, the build files, or the physical
serial capture. Where the workbook and the firmware disagree, I say so and give
the file and line.

---

## 0. The finding that reframes the whole workbook

The workbook's own **Sources + Definitions** sheet defines `Implemented` as:

> "Current UI/source/tests contain the intended capability with no known core
> contradiction. … **Does not mean shipping-ready.**"

and the **Obsolete + Conflicts** sheet flags it explicitly:

> "Misleading claim | Implemented vs verified | Live diagnostic labels source
> presence | **All reproduction rows were NOT RUN** | Keep build state separate
> from proof state | Current app can look more complete than it is"

So `Implemented` in the Capability Matrix, Control Map, FX Matrix and
MIDI + Heads sheets means **the browser MVP contains it**. It says nothing about
the SP-1.

The numbers make this concrete:

| Slice | Count |
|---|---:|
| CAP rows total | 225 |
| CAP rows whose SP-1 path is **"New firmware work"** | **94** |
| …of those, already marked `Implemented` (in the browser) | **31** |
| AUD rows (35) whose Reproduction field is `NOT RUN` | **35 / 35** |
| FW rows (25) in state `Missing` or `Mock only` | **13** |
| Workbook's own "verified behavior" score | **0.027** |

The **Firmware Slice** sheet is the only sheet that scores the physical device,
and it is the one telling the truth. `FW-16 Offline playback — Missing`,
`FW-17 Shared clock — Missing`, `FW-20 Base speed — Missing`,
`FW-21 FX latch — Missing`, `FW-22 Cue MIDI — Missing`,
`FW-23 Heads — Missing`, each with evidence "Browser model only".

**The instrument exists in TypeScript. The SP-1 is a four-stem file player.**
That is the gap, and everything below is the shape of it.

---

## 1. What the firmware actually does today — verified, not claimed

I traced every path from `main.c` outward. This table is source-verified.

| Subsystem | Reaches the real audio output? | Evidence |
|---|---|---|
| Bulk upload, A/B commit, magic-last index | **Yes — physically proven** | 248.5 MiB committed, generation 2→3, 509,024/509,024 blocks verified, 0 retries |
| Cold-boot index parse + song select | **Yes — physically proven** | boot line `V11 lib: gen_hi=0 gen_lo=3 active_index=0 active_song=1` |
| Four-stem decode + stereo mixdown | **Yes** | `st_stem_mix_frame()` in PASS C, `main.c:2413` |
| Per-stem mute / solo / fader gain | **Yes** | `main.c:2249–2251` feeds `stem_channels[]` into the stem mix |
| Play / stop | **Yes** | `g_playing` gates `stem_active`, `main.c:2323` |
| Beat phase data (BPM/downbeat) | **Yes (data only)** | `st_beat_phase` wired; LED consumer is Phase 4, unbuilt |
| **Varispeed (Mode 2) on a stem song** | **NO** | see §2 |
| **Semitone snap on a stem song** | **NO** | see §2 |
| **Tape inertia on a stem song** | **NO** | see §2 |
| **Reverse (any scope)** | **NO** | no implementation anywhere in firmware |
| **Scrub / shuttle** | **NO** | `st_scrub.c` is not compiled into the target |
| **Lane loops, double-tap, chords, FX scope** | **NO** | `st_gesture.c` is not compiled into the target |
| **Any of the 12 FX algorithms** | **NO** | no DSP exists; `st_fx_catalog.h` is a table, not compiled |
| **Heads (4 readers)** | **NO** | nothing exists |
| **Cue MIDI learn / momentary play / return** | **NO** | `st_midi_queue.c` decodes UMP words and queues them; no cue model |
| LED semantics | **Partial** | `main.c` drives GPIO + soft-PWM directly; `st_led_pattern.c` / `led_render.c` are not compiled |

---

## 2. Varispeed, semitone and inertia are stranded — correcting the workbook

You told me these must survive. They exist in the tree, but I have to be exact
about what "exist" means, because the workbook is wrong here and it matters.

The Control Map says:

> `CTL-13 | Rocker | Hold/press | Base Mode 2 varispeed; pitch follows | Implemented`
> `CTL-14 | Rocker | Double-tap | Reset exact 1× | Implemented`

The Firmware Slice says:

> `FW-20 | Base speed | Mode 2 varispeed and pitch behavior preserved | **Missing** | Web implementation only`

**FW-20 is correct and CTL-13/14 are wrong for the physical device.** Here is why:

- The speed machinery is real and intact: `g_play_speed_q16` (`main.c:1395`),
  the smoothed `g_cur_speed_q16` with the one-pole `sd / 50` tape inertia
  (`main.c:1978–1981`), the Q16 phase accumulator `g_pphase`, and the
  25-entry semitone table `k_semi_q16[25]` (`main.c:4930`).
- All of it feeds **PASS A/B**, which computes `mix32[]` from `trk[].pring`.
- **PASS C — the stem path — never reads any of it.** It advances with
  `st_stream_advance_frame()` (`main.c:2419`): exactly one stored frame per
  output frame. Fixed 1×, forward only.
- And `mix32[]` is provably silent: `present[]` is never assigned a nonzero
  value anywhere in `main.c`, so `trk[].state` can never reach `TS_PLAY`. There
  is a CI gate (`.github/scripts/stemtape_player_classic_source_absence_gate.py`)
  that enforces exactly this.

So on your device right now, **turning the rocker while a stem song plays does
nothing to the audio.** The code is not gone — it is wired to a bus that is
architecturally guaranteed to be silent.

This changes the RAM plan in an important way, and it is the reason I was wrong
to describe removing the classic engine as a straightforward reclaim:

> **Extracting varispeed/semitone/inertia into a real, resampling stem playhead
> is not cleanup that follows the RAM reclaim. It is the prerequisite for it,
> and it is a feature the product does not currently have at all.**

---

## 3. Dead weight — the honest split

You said: *"I want ZERO dead weight in the code and firmware, ZERO."*
There are two completely different kinds here and conflating them would cost us.

### 3a. Dead RAM in the shipped image — this is the real cost

The linker's own figure, from the green CI build of `e8e189b`:

```
Memory region     Used Size   Region Size   %age Used
        FLASH:      99308 B        892 KB      10.87%
          RAM:     228574 B        256 KB      87.19%
RAM budget OK: 228574 bytes used, 33570 bytes free
```

Every allocation in `main.c` below is enumerated with its array dimensions
resolved from the real headers; the kernel/driver line is then **derived by
subtraction from the linker total**, not estimated.

| Item | Bytes | %RAM | Verdict |
|---|---:|---:|---|
| `trk[4]` — classic play rings `pring[16384]`×4 + per-track fields | **131,328** | 57.5% | **Dead** — only PASS A/B read them, and PASS A/B is provably silent |
| `batchbuf[32×512]` (`main.c:3801`) | **16,384** | 7.2% | **Dead** — classic play-ring refill staging; dies with `pring` |
| `tx_slab` — I2S DMA output, 10 × 1024 B | 10,240 | 4.5% | Live — 53 ms output cushion |
| `g_cdc_rx` upload receive ring | 8,274 | 3.6% | Live — **upload only** |
| `s_v11_verify_scratch[8192]` | 8,192 | 3.6% | Live — **upload only** (read-back verify) |
| `g_stem_sector_buf[8192]` | 8,192 | 3.6% | **Live — this is playback** |
| `g_stem_sector_buf_b[8192]` | 8,192 | 3.6% | **Live — this is playback** |
| Thread stacks in `main.c` (audio 3072, streamer 4096, midi 768) | 7,936 | 3.5% | Live |
| `posb` / `fracb` / `mix32` (`main.c:2009–2011`) | **2,560** | 1.1% | **Dead** — classic resampler scratch |
| `metabuf[4×512]` (`main.c:3796`) | **2,048** | 0.9% | **Dead** — reads the classic index at boot |
| `sec[512]` (`main.c:3469`) | 512 | 0.2% | Live — single-block upload staging |
| `blk[512]` (`main.c:3795`) | **512** | 0.2% | **Dead** — classic scratch |
| `g_meta` (`struct meta_blk`, `main.c:930`) | **64** | — | **Dead** — classic 16-slot loop index, superseded by STIX |
| **Everything else** — Zephyr kernel, USB device stack, CDC ACM, USB MIDI2, I2S/ADC/I2C/WDT drivers, main + ISR + idle stacks | **24,140** | 10.6% | Live |
| **Total (matches the linker exactly)** | **228,574** | 100% | |

Read that table by what it says about **playback**, because that is the
question worth asking:

> **Playing a song costs 16,384 bytes — 7.2% of RAM.** Two sector buffers.
> Nothing else in the image belongs to playback at all.

Nothing here is bloated. The entire Zephyr kernel plus every driver — USB,
CDC, MIDI, I2S, ADC, I2C, watchdog — comes to 24,140 bytes, **10.6%**. (My
earlier estimate of "~48,000" for that line was roughly 2× too high; it was a
guess, and this table replaces it with arithmetic.)

What actually occupies the device is a tape looper that can never make a
sound:

| Bucket | Bytes | %RAM |
|---|---:|---:|
| **Classic Tape Looper engine — provably silent** | **152,896** | **66.9%** |
| Upload path (resident even while playing) | 16,978 | 7.4% |
| **Stem playback** | **16,384** | **7.2%** |
| I2S output slab | 10,240 | 4.5% |
| `main.c` thread stacks | 7,936 | 3.5% |
| Zephyr kernel + all drivers + remaining stacks | 24,140 | 10.6% |

**Reclaimable: 152,896 bytes ≈ 149 KB — 66.9% of RAM in use.** Everything the
device genuinely does today — play a song, receive an upload, drive I2S, run
Zephyr — fits in **75,678 bytes, 29% of the 256 KB region**, leaving ~180 KB
free.

For scale: 149 KB buys **18 more 8192-byte sector buffers**. That is the
entire budget for Heads and for the read-ahead depth that makes dropouts
structurally impossible. And it reframes the dropout problem — playback is not
using too much memory, it is using far too little: two buffers is 14.2 ms of
audio against a 16.1 ms worst-case read (§4, Decision 3).

Two live items are worth revisiting once the reclaim lands, though neither is
urgent next to 149 KB:

- `s_v11_verify_scratch` (8,192 B) is used **only during upload**, when
  playback is stopped. It can overlay a stem sector buffer instead of holding
  its own allocation for the device's whole life.
- `tx_slab`'s comment claims a "~106 ms DMA cushion". At 10 × 256 stereo
  frames it is **53 ms** — the 106 ms figure is left over from the 24 kHz
  (`DECIM=2`) build. Stale comment, not a bug.

Note the precedent is already in this file — `main.c:1040`, where I removed
`g_rring` and wrote *"This reclaims 32 KB of RAM."* Same reasoning, nearly
five times the size.

### 3b. Dead source in the tree — costs zero RAM, and mostly must NOT be deleted

I built the include graph from `main.c` outward. **26 of 57 source files —
3,882 lines — are unreachable from `main.c`.**

Critically: `firmware/stemtape_player/CMakeLists.txt` **does not compile them
into the target**. They cost zero flash and zero RAM. I verified this against
the `target_sources(app PRIVATE …)` list, which names 15 files.

| Island | Lines | What it actually is | Ruling |
|---|---:|---|---|
| `st_gesture.c/.h` + `st_fx_catalog.h` | 842 | The **entire control-matrix state machine** — chords, holds, double-taps, FX scope, lane loops | **KEEP AND WIRE.** This is CTL-01…CTL-39. Deleting it is exactly the mistake you stopped me from making with varispeed. |
| `st_scrub.c/.h` | 175 | Scrub speed/ramp/release + zero-crossing | **KEEP AND WIRE.** CTL-05…CTL-10, AUD-18…AUD-21. |
| `st_led_pattern.c/.h` | 442 | LED semantic priority/pattern model | **KEEP AND WIRE.** Phase 4, CAP Feedback domain (17 rows). |
| `led_render.c`, `led_render_policy.c`, `led_duty.c/.h`, `led_protocol.h` | 656 | 8-channel PWM renderer + duty policy | **Decide.** `main.c` already drives LEDs directly. Either adopt this renderer or delete it — but not both. |
| `st_storage_layout`, `st_sector_codec`, `st_transfer`, `st_library_io`, `st_xfer_wire`, `st_stem_validate` | 1,767 | The **retired v1.0 transfer contract**, fully superseded by v1.1 (`st_bulk_xfer` + `st_ab_session` + `st_stix`) | **DELETE.** This is the only genuinely obsolete island. It is kept today for regression evidence, but v1.1 is now *physically proven* on hardware — the evidence has been superseded by something strictly better. |

So the real answer to "zero dead weight" is: **~149 KB of RAM and 1,767 lines of
retired v1.0 transfer code are dead. The other 2,115 lines are the product,
sitting unwired.** The workbook's own warning applies to our own tree — unwired
modules present as more complete than they are.

---

## 4. The three coupled decisions that gate everything

Nothing in Heads, reverse, scrub or dropout-elimination can be designed until
these are answered, and they are answered in this order.

### Decision 1 — Is the eMMC actually on SPI? (blocks the other two)

Measured on your device: `rate≈1,160,000 B/s`, `rdus≈6,340 µs` avg,
`rdusmx=16,137 µs` worst.
Required for plain 1× playback: 48,000 frames/s × 24 B = **1,152,000 B/s**.

**That is 0.7% headroom.** It is the direct cause of the dropouts, and no amount
of buffering fixes a stream that cannot be read faster than it is consumed —
buffer depth only changes how long it takes to fail.

The diagnostic prints `spim=1`. If the eMMC is being driven over SPI rather than
native 4-bit MMC, the ceiling is roughly 10–50× lower than the part is capable
of. **This is unverified and it is the single most important measurement in the
project.** Every downstream number changes by an order of magnitude depending on
the answer.

*Next action: read the devicetree/overlay bus binding and the `sp1_emmc.c`
transfer path, then bench a raw sequential read at maximum clock.*

### Decision 2 — Storage layout: interleaved vs de-interleaved

Today one frame is 24 bytes: 4 stems × stereo × 3 bytes, **interleaved**.

That is optimal for "play all four stems at 1× forward" and pessimal for
everything the product is actually about:

| Scenario | Bytes/s required with interleaving | With per-stem regions |
|---|---:|---:|
| Normal 1× four-stem playback | 1,152,000 | 1,152,000 |
| One stem soloed | 1,152,000 (must read all 24 B) | 288,000 |
| **Heads: 4 readers into the vocal stem** | **~4,608,000** | **~1,152,000 + 864,000 = 2,016,000** |

Measured capacity is 1,160,000 B/s. **Heads is 4× over budget interleaved and
1.7× over budget de-interleaved.** Neither works until Decision 1 is resolved.

This is a **format decision**, and the format is now committed to a physical
device that took you 43 minutes to load. Changing it means a re-upload. That
cost is exactly why Decision 1 must come first — if the bus is the limit and
lifting it gives 10×, interleaved survives and no re-upload is needed.

### Decision 3 — N-position bidirectional streaming

The current engine is a two-buffer, single-position, forward-only prefetch.
(And until commit `e8e189b` it was not even prefetching — it published the
*current* sector as the request, so `und == rdc` exactly 1:1 and you were
hearing roughly a 10% duty cycle of real audio. That is fixed but not yet
flashed on your device.)

What the workbook actually requires of the streamer:

- **Heads** (`HEADS` sheet, 14 rows, all "New firmware work"): four
  simultaneous read positions into the vocal stem at Current / ¼ / ½ / ¾ song
  offsets, each with independent level, mute, **reverse** and **scrub**, with
  last-touched foreground arbitration, while drums/bass/instruments continue on
  the hidden song clock.
- **Reverse** appears Front-line in three separate places: `CAP-061` per-stem
  reverse, `CAP-126` per-head reverse, `CAP-034` reverse during wind-down, plus
  `CTL-11` and `CTL-36`. It requires descending and direction-aware prefetch —
  the current streamer can only count up.
- **Varispeed** requires fractional-rate reads, so read demand becomes a
  function of speed: at 2× it is 2,304,000 B/s from a bus that measured
  1,160,000.

So the target is: **N independent, direction-aware, fractional-rate read
positions over a shared sector cache**, not a two-buffer ping-pong. That is a
rewrite of `st_stem_stream.c`, and the 149 KB from §3a is what pays for it.

---

## 5. Domain-by-domain gap (all 225 CAP rows)

| Domain | Rows | Firmware reality | Gap |
|---|---:|---|---|
| Ingest | 17 | **Done, physically proven** | Resume (paused); device-side library view |
| Transfer | 7 | **Done, physically proven** | Workbook says `Mock only` — **stale**, supersede those rows |
| Format | 6 | **Done** | Re-open only if Decision 2 flips |
| Storage | 3 | Cold-boot index **works** | `CAP-199` says `Missing` — **stale**, boot line proves it |
| Firmware safety | 4 | M0 gate + v1.1 write gate live | `FW-01` gate conflict: resolved |
| Transport | 9 | Play/stop live; song select partial | On-device song browse, cue-to-start |
| Mix | 12 | Mute/solo/gain **live on the stem bus** | Click-free ramps, link (`CTL-25`) |
| Speed | 5 | **Stranded** (§2) | Resampling stem playhead |
| Tape performance | 21 | **None** | Scrub, shuttle, lane loops, reverse, park |
| Heads | 19 | **None** | Blocked on Decisions 1–3 |
| FX + FX algorithm | 25 | **None** | 12 algorithms, no DSP exists |
| Cue MIDI | 15 | UMP decode only | Learn / momentary / hidden-clock return |
| Feedback | 17 | Direct GPIO; beat pulse unbuilt | Phase 4 + wire `st_led_pattern` |
| Analysis | 5 | BPM/downbeat consumed from STIX | Companion-side; on-device is data-only |
| Projects / Product model / Web UI | 25 | Companion | Device library view is the live ask |
| Hardware | 9 | Power, DFU, battery live | **`CAP-211` RAM/CPU budget — §3a is the answer** |
| Diagnostics / Validation / Firmware proof | 10 | Extensive | `FW-15`…`FW-23` need bench capture |
| Stock reference (`AUD-02`, `AUD-35`) | 2 | **Captured 2026-08-20** | See §5a — no longer `Unverified` |
| **Recording (obsolete)** | **13** | Correctly unreachable | **`CAP-213`…`CAP-225` — delete, per your ruling** |

---

## 5a. The stock SP-1 baseline — captured 2026-08-20

The workbook has carried three rows waiting on this and nothing else:
`AUD-02` *"Startup stock behavior/signature — reference behavior not
established"*, `AUD-35` *"Stock SP-1 reference — reference behavior not
captured"*, and Sources authority #10 *"exact parity still needs captured
baseline"*. They are no longer unverified. An unmodified retail SP-1 was
enumerated on macOS (`system_profiler SPUSBDataType` + `ioreg -w0 -l -r -n
"stem player"`); this is what it is:

```
Product name        "stem player"
Vendor ID           0x2367 (teenage engineering)
Product ID          0x1701
bcdDevice           0x0100
bcdUSB              0x0200        USB 2.0 compliant
Device Speed        1             FULL SPEED (12 Mb/s)
bMaxPacketSize0     64
bDeviceClass        0             defined at interface level
bNumConfigurations  1
Current required    500 mA

  Interface 0  (the only one)
    bInterfaceClass     255       0xFF, VENDOR-SPECIFIC
    bInterfaceSubClass  0
    bInterfaceProtocol  0
    bNumEndpoints       0         <-- zero
```

**`bNumEndpoints = 0` is the whole finding.** A single vendor-specific
interface with no endpoints beyond the mandatory control endpoint means
every byte a host exchanges with a stock SP-1 travels over **EP0 control
transfers**. By descriptor, that rules out — with no interpretation
required — MIDI (needs interrupt/bulk), audio (needs isochronous),
CDC/serial (needs bulk), mass storage and HID.

Four consequences, in descending order of how much they change our plan:

1. **There is no stock reference for the Cue MIDI path.** The hope was that
   stock exposed a class-compliant MIDI interface we could match for the
   KO II work (`FW-22`, the 15 Cue MIDI CAP rows). It does not — TE's design
   has no MIDI endpoint at all. Our `CONFIG_USBD_MIDI2_CLASS` approach is
   entirely our own invention, and "match stock" is **not available as a
   fallback** if it gives trouble. Worth knowing before that work starts,
   not during it.

2. **"Run the serial diagnostic on a stock unit" is impossible, not merely
   empty.** Stock presents no CDC interface, so no `/dev/cu.*` appears at
   all (verified directly: the port list is byte-identical unplugged and
   plugged in). There is nothing to listen to. Any future stock comparison
   has to be descriptor-level or audio-level, never console-level.

3. **Stock is Full Speed, exactly like ours.** `bcdUSB 0x0200` declares USB
   2.0 while `Device Speed = 1` reports 12 Mb/s — the nRF52840's USB
   controller is full-speed-only, so TE ships at the same ceiling we do. We
   concede nothing to stock on the transfer path, and the ~1.2 MB/s
   theoretical USB bulk cap applies to both.

4. **No VID collision is possible.** Stock is `0x2367:0x1701` (teenage
   engineering's own vendor ID). The classic Tape Looper is `0x1915:0x5210`
   and this firmware is `0x1915:0x5211`, both under Nordic's VID. Stock and
   modded differ at the *vendor* level, not just the product level, so a
   host driver cache can never confuse the two. prj.conf's "distinct PID"
   note is satisfied more strongly than it claims.

Method note for anyone repeating this: `system_profiler` emits
`IOCreatePlugInInterfaceForService failed 0xe00002be`
(`kIOReturnNotPermitted`) on stderr for devices it cannot open a plug-in
interface for. That is a permissions warning, not a failure — stdout is
still complete. `ioreg` reads the IORegistry directly and does not hit it,
so prefer `ioreg -w0 -l -r -n "stem player"` when the two disagree. Also
expect a browser to appear holding an `AppleUSBHostDeviceUserClient` on the
device if any WebUSB page has claimed it.

---

## 6. Conflicts that need a one-sentence ruling from you

The **Obsolete + Conflicts** sheet lists these as blocking. I am not going to
guess on them; each changes what gets built.

1. **Heads PRINT** — offline bounce of a Heads performance. Recording is out,
   but is an offline *render* recording? (`HEADS/PRINT`, "Explicit ruling required")
2. **Isolator vs Exciter** — old bank had Isolator, current has Exciter. Replace or keep both? (`LEG-01`)
3. **Beat Repeat and Pump** — in the old Rhythm bank, absent from the current 12-slot map. Drop or re-slot? (`LEG-02`, `LEG-03`)
4. **Manual tap grid vs automatic analysis** — current app does automatic BPM/phase; does manual correction survive?
5. **The two-track power-off chord you hit.** Pressing far-left + far-right tracks powered the device down. Solo is **hold ONE track ≥700 ms** (`CTL-03`, `TRACK_HOLD_SOLO_MS`). The inherited Tape Looper chord needs suppressing — I will treat this as a bug and fix it unless you say otherwise.

---

## 7. The build order

Strictly sequenced, because each step pays for the next.

**Step 0 — Measure the bus.** Resolve `spim=1`. Nothing about Heads, layout, or
buffer depth can be designed honestly before this number exists.

**Step 1 — Extract the tape playhead.** Pull varispeed, pitch-follows-speed, the
`sd/50` inertia and `k_semi_q16` out of the classic engine into a pure,
host-tested module. This *preserves* what you told me to preserve — as a real
capability instead of a stranded one.

**Step 2 — Wire it to the stem path.** `st_stream_advance_frame()` becomes
rate-driven and direction-aware. Varispeed and semitone become real on your
device for the first time. Reverse becomes reachable.

**Step 3 — Reclaim the 149 KB.** Only now is removing `pring[]`, the classic
mixer scratch, `g_meta` and `batchbuf` safe — nothing needed survives in there.
Delete the 1,767-line retired v1.0 transfer island at the same time.

**Step 4 — Re-spend it.** Deep multi-sector read-ahead. This is where "no
dropouts, ever" stops being a hope and becomes a budget: read-ahead depth ×
sector duration must exceed `rdusmx` worst-case read latency, with margin.
At 7.08 ms per sector and a 16.1 ms worst-case read, the current 2-buffer
design has **negative** margin. That is the arithmetic behind the dropouts.

**Step 5 — Wire the control matrix.** `st_gesture.c` gets its real caller.
CTL-01…CTL-39 become live. This is task #52, pending since the beginning.

**Step 6 — Heads.** N-reader streamer on the reclaimed RAM, with the layout
decided by Step 0's measurement.

Steps 1–4 are also, and not coincidentally, the fix for "there should be NO
dropouts of audio EVER."

---

## 8. What I got wrong earlier, corrected here

- I described removing the classic engine as a clean RAM reclaim. It is not —
  varispeed, semitone and inertia have to be **extracted and given a real home
  on the stem bus first**, because they have never worked there. You caught
  this; §2 is the full accounting.
- I omitted Heads and reverse from a removal plan. They are 19 CAP rows plus
  5 control-map rows, and reverse is Front-line in three separate places. §4
  and §5 carry them now.
- I called 3,882 lines of unreachable source "dead weight" without checking the
  build. It is not compiled into the firmware and costs zero RAM; only 1,767
  lines of it are genuinely obsolete. §3b splits it properly.
