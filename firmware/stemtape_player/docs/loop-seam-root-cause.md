# The loop seam: why st17 blipped, and what the base SP-1 does instead

**Status: root cause and measurement only.** Nothing in this document is
wired into the audio path yet — that is the next commit. Nothing here is
physically verified; every number was produced on a host from the frozen
fixture.

---

## 1. What the hardware reported

Flashing st17 and looping on the physical SP-1 produced:

* an audible blip/outage **entering** the loop,
* every **wrap** sounding "like an obvious seek" rather than a loop,
* another audible outage on **release**,
* and the observation that **the base SP-1's loop is substantially smoother**.

That last point is the important one. It rules out the whole class of
explanations this firmware had been reaching for, because the base SP-1 runs
on the same eMMC, the same driver, the same I2S path and a much smaller
buffer budget. If depth were the problem, the base would be worse, not
better.

## 2. The defect

**A loop seam is a step discontinuity in the waveform, and st17 had no
mechanism of any kind for it.**

At a seam the firmware plays frame `end-1` and then frame `start`. Those two
frames are adjacent in the output and arbitrarily far apart in amplitude —
they are unrelated points in the music. The output therefore contains an edge
the material never contains, which is heard as a click. This happens with:

* every frame present,
* nothing repeated,
* nothing skipped,
* every sector resident,
* zero underruns.

Measured on the frozen four-stem fixture, reproducing st17's exact schedule
(`tests/test_loop_seam_gate.c`, "ducker disengaged"):

| transition | output step | the material's own largest local step |
|---|---:|---:|
| entry | 6 774 | 64 036 |
| wrap  | 9 557 | 64 036 |
| exit  | 10 361 | 64 036 |

with **0 repeated and 0 skipped frames** anywhere. The steps are well below
the fixture's largest *natural* step, which is why "compare against the
loudest transient in the song" is a useless threshold — but they occur where
the music is not making a step, which is what makes them audible.

**No amount of buffer depth removes this.** Residency fixes starvation. This
is not starvation. Adding pinned sectors to chase it is exactly how these
pools reached the size the RAM audit is now unwinding.

## 3. The base SP-1's real implementation, inspected

Source: `firmware/src/main.c` (the golden Tape Looper, read-only).

The mechanism is called **BOUNDARY FADE** and lives inside the per-frame
mixer loop at **lines 1962–1972**:

```c
/* BOUNDARY FADE (unchanged): fade out over the last ~5 ms as
 * the ring drains, fade in after recovery — dropouts duck
 * instead of clicking. */
{
        int32_t g = 256;
        if (avail < 256) g = avail;                    /* fade OUT  */
        if (trk[i].fade < 256u) {
                if ((int32_t)trk[i].fade < g) g = (int32_t)trk[i].fade;
                trk[i].fade++;                         /* fade IN   */
        }
        if (g < 256) sv = (int16_t)(((int32_t)sv * g) >> 8);
}
```

Its whole state is one field, declared at **line 950**:

```c
uint16_t fade;   /* starve-recovery fade-in position (256 = full; mixer-only) */
```

armed at **line 1942**:

```c
trk[i].fade = 0;   /* ramp back in (~5 ms), no click */
```

and the same idea is applied to the record-time loop seam at **line 1492**:

> *"the pad used to be hard zeros — a click baked into the seam; fade the
> first 128 pad samples (~2.7 ms) down instead."*

### Answering each question the directive asked, from the source

| question | answer, from `firmware/src/main.c` |
|---|---|
| overlap / crossfade length | **No crossfade.** A one-sided gain ramp. 256 frames (~5.3 ms) for starve recovery; **128 frames (~2.7 ms)** where it is specifically a *loop seam* (line 1492). |
| fade curve | **Linear**, in integer steps of 1 out of 256, applied as `(sv * g) >> 8`. No curve table, no windowing function. |
| two playheads? | **No.** One playhead. `sv` is the single interpolated sample for that track at that frame. |
| do outgoing and incoming overlap? | **No.** They are never summed. The gain is taken to zero, the position changes, the gain comes back. |
| how is release handled? | The same ramp, armed by setting `fade = 0` and letting the mixer walk it back to 256. There is no separate release path. |
| what scratch memory does it reuse? | **None.** One `uint16_t` per track and one multiply in a loop that was already running. Zero buffers, zero sectors. |
| does it modify I2S transport state? | **No.** It is entirely inside `looper_audio_block()`'s mixer. `i2s_configure` / `i2s_trigger` / `i2s_write` are untouched; the block is produced and written exactly as always. |

That last row is the one that matters most for cost. The proven technique is
free: it does not add a buffer, does not add a playhead, does not add
residency, and does not touch the transport.

`src/st_seam.h` is that mechanism, expressed once for the four-stem stream:
a phase, a step counter, and a Q8 gain. `ST_SEAM_FRAMES` is 128 — the base
SP-1's own loop-seam length, not a new number.

## 4. The two call chains

### Base SP-1 — `firmware/src/main.c`

```
audio_thread()                                   (line 3352)
  └─ k_mem_slab_alloc(&tx_slab, &blk)
  └─ looper_audio_block(blk)
       └─ PASS A  per-frame transport / record
       └─ PASS B  per-track mixer, for f in 0..BLK_FRAMES
            ├─ read ring, interpolate           →  sv
            ├─ BOUNDARY FADE                    (line 1962)   ←── THE SEAM
            │    g = min(256, avail, trk[i].fade++)
            │    sv = (sv * g) >> 8
            └─ mix32[f] += (sv * vf) >> 8
       └─ PASS C  master, clip, write s[]
  └─ i2s_write(i2s_dev, blk, BLK_BYTES)          ← transport untouched
```

The seam is a multiply in the innermost loop of a pass that already existed.

### Stem Tape st17 — `firmware/stemtape_player/src/main.c`

```
audio_thread()                                   (line 5038)
  └─ looper_audio_block(blk)                     (line 2350)
       └─ stem_audio_block(s, m0, md, mv)        (line 2045)
            while (f < BLK_FRAMES):
              ├─ sample loop window atomics      (line 2124)
              ├─ ENTER  atomic_cas(g_stem_loop_enter_req)
              │            └─ st_stream_seek(enter_fr)         ←── STEP, un-ducked
              ├─ EXIT   atomic_cas(g_stem_loop_exit_req)
              │            └─ st_stream_seek(resume_fr)        ←── STEP, un-ducked
              ├─ residency: pin lookup, else mailbox acquire
              ├─ clamp run to sector / song / block / loop-end
              ├─ stem_render_run(buf, fis, ..., f, run, s, peak)   (line 1852)
              └─ WRAP   song_frame >= lp_hi
                           └─ st_stream_seek(lp_lo)            ←── STEP, un-ducked
  └─ i2s_write(...)
```

Three `st_stream_seek()` call sites, three step discontinuities, and no gain
stage anywhere between the decode and the master volume. That is the entire
defect: **the base SP-1's mixer has a seam multiply and Stem Tape's does
not.**

### Where the repair goes

`stem_render_run()` (line 1852) is the only place samples are produced, and
it already takes the master-volume arguments `m0, md, mv`. The seam gain
composes with those — one more multiply on a value that is already being
multiplied. The three seek sites become *requests*; the jump happens on the
frame `st_seam_jump_due()` reports the gain has reached zero.

No new buffer. No second playhead. No change to `i2s_configure`,
`i2s_trigger` or `i2s_write`. That is Commit B.

## 5. What the previous harness failed to model, exactly

`tests/test_loop_playback_gate.c` passed st17 with "zero silent frames at
entry, wrap and exit". It was not lying. It modelled:

* which **sector index** the stream required each iteration,
* whether that sector was **resident** (pin or ring),
* whether a frame was **skipped or repeated** across a seek,
* whether a block contained **silence** from an underrun.

Every one of those was correct and remains correct. What it never did was
**read a sample value**. Its notion of "the audio is fine" was
`frame[n+1] == frame[n] + 1`, which is exactly the property a seam
*preserves*. A step discontinuity is invisible to an index-based model by
construction.

Four further things it did not model, each of which the new gate does:

1. **Sample values.** It had no decoder in the loop at all.
2. **I2S-block-sized rendering.** It advanced by runs, not by fixed 256-frame
   blocks, so it could not see anything that happens *at* a block boundary.
3. **Request arrival offsets.** Control requests were applied at run
   boundaries. On hardware they arrive at whatever frame offset inside a
   256-frame block the renderer happens to be at — 256 distinct cases.
4. **Concurrent transitions.** A release arriving while a wrap is still in
   flight was never generated. That is the case that broke the first version
   of the *new* gate too (see §6).

## 6. What the new gate measures

`tests/test_loop_seam_gate.c`. It loads
`handoff/v1.1/binaries/song-sectors-four-stem.bin` (43 sectors, 14 620
frames), decodes through `st11_sector_decode_frame()` and a **local** unity
mix, and measures first differences in the output.

The reference is deliberately independent: the production mixer is not
called anywhere in the file. The production side supplies only the
*schedule* — which source frame is emitted when, and at what seam gain —
which is the thing under judgement.

The threshold is the material's own largest first difference in the
**neighbourhood** of each seam (±2048 frames). A full-fixture maximum was
tried first and rejected: at 64 892 it passes anything, because this fixture
swings near full scale somewhere.

Results with the ducker engaged:

| transition | st17 | ducked | improvement |
|---|---:|---:|---:|
| entry | 6 774 | 149 | 45.5× |
| wrap  | 9 557 | 176 | 54.3× |
| exit  | 10 361 | 0 | — |

with 0 repeats and 0 skips, the exit still landing exactly on
`loop_end_exclusive`, and no frame of the looped section heard again after
the release.

Cases, all against the frozen fixture:

* **the material's natural step** — context, and proof the global maximum is
  the wrong threshold;
* **st17 reproduced** — the defect must keep reproducing, or the harness has
  stopped modelling what failed. CI greps for this line;
* **the ducker engaged** — the table above;
* **24 seam alignments** — loop start on the first frame of a sector, the
  frame after it, either side of mid-sector, and the last frame of a sector,
  crossed with lengths putting `loop_end` on a sector boundary, one frame
  either side of one, and nowhere near one. 96 of the resulting seam joins
  land in a different sector from the frame before them, so the sweep really
  does exercise sector-crossing seams rather than assuming it does;
* **release destination** — the first post-loop frame is `loop_end`, and no
  frame of the looped section is ever heard again;
* **all 256 request offsets in a 256-frame block** — entry and release
  requested at every offset, checking the step at each, that the seam gain
  crosses every block boundary continuously (≤ one ramp increment, so the
  ducker is provably not re-derived per block), and that no frame escapes the
  window;
* **16 consecutive wraps** — a latched loop left running; no wrap degrades
  with repetition;
* **length changes mid-loop** — including two shrinks that strand the
  playhead outside the new window, and a grow;
* **residency cost** — distinct sectors per 256-frame block, measured with
  the ducker on and off. Both are 4. Equal is the proof: the outgoing and
  incoming audio never sound together, so smoothing adds no playhead, no
  buffer and no residency.

### Two defects the new gate found in itself

Recorded because both are mistakes production must not repeat.

1. **Arming the wrap duck at the boundary instead of before it.** The wrap is
   the one transition known in advance. Armed *at* `loop_end` the gain is
   still near unity when the jump happens and almost none of the step is
   removed. Armed `ST_SEAM_FRAMES` *before* it, the jump lands at zero gain:
   wrap 8 753 → 238.

2. **Jumping after a fixed frame count instead of on `st_seam_jump_due()`.**
   When a release arrives while a wrap's duck-in is still running,
   `st_seam_begin()` mirrors the partially-completed UP phase
   (`step = FRAMES - step`) so the gain reaches zero *sooner* than
   `ST_SEAM_FRAMES` frames later. Counting overshot the zero point, the jump
   landed with the gain already climbing back toward unity, and the measured
   exit step went **10 361 → 35 219 — 3.4× worse than no ducker at all**.
   Fixed by routing all three transitions through one request/`jump_due`
   pump. That is the API `st_seam.h` defines, and the reason it defines it.

## 7. What is not claimed

* The seam is **not** wired into the audio path yet.
* Nothing here has been heard. These are host measurements of sample values.
* CI passing this gate means the waveform is continuous in the model. It does
  not mean the loop is seamless on the SP-1, and no CI output in this
  repository may say that it does until the flashed BIN is physically
  accepted.
