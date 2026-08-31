# Stem Tape RAM — where it goes, and how to get it back

`main.c` has referenced this document for some time; it did not exist. Written
now because the roadmap (per-track scrub heads, multi-song, heads mode, MIDI
cue) needs headroom that does not currently exist, and because song-planar
v1.2 needs +16 KB before it can be built at all.

Every number below is computed from the compiled constants
(`ST_LAT_READAHEAD_SECTORS 4`, `ST_LAT_RESIDENCY_SECTORS 5`,
`ST_LAT_RING_SLOTS 6`), not estimated.

## Where the RAM actually is

228,574 B used of 262,144 B. **Three arrays are 139,264 B of it — 61%.**

| pool | slots | bytes | purpose |
|---|---|---|---|
| `g_stem_sector_bufs` | 6 × 8192 | 49,152 | read-ahead ring |
| `g_stem_loop_pin_bufs` | 10 × 8192 | 81,920 | loop entry + exit residency |
| `s_v11_verify_scratch` | 1 × 8192 | 8,192 | upload read-back verify |
| | **16 sectors** | **139,264** | |

Everything else in the firmware — every control struct, every LED table, every
diagnostic counter — is the other 89,310 B combined. `st_stream_t` is 36 bytes,
`st_stem_mbox_t` is 32, `st_stem_meter_t` is 8. **There is no meaningful RAM
outside the sector pools**, so a reclamation that does not change them cannot
succeed, and one that does needs nothing else.

### Stale code is not the opportunity

Worth stating plainly, because it is the intuitive place to look. The classic
Tape Looper's `pring[]` — 4 × 16384 × 2 = **131,072 B** — was already removed
in the earlier classic-reclamation pass, and its comment in `struct looptrk`
records that. What remains of the classic engine is scalars: `struct looptrk`
× 4 tracks, `g_grid_*`, a handful of request flags — low hundreds of bytes.
`sp1_emmc.c` holds 517 + 514 + 512 = ~1.5 KB of DMA and CRC tables, all live.

The 139,264 B is not dead weight. It is live buffering, correctly sized for the
policy it implements. **The policy is what has to change.**

## Why the loop-pin pool exists, and why it is the target

From its own comment in `main.c`:

> …the read-ahead ring maps sector s to slot s % SLOTS, so a window wider than
> the ring cannot hold both of its ends — an artefact of the ring's addressing,
> not a property of the problem. A unified cache with associative lookup and
> pinnable slots removes them entirely.

That is the whole diagnosis. A modulo-addressed ring cannot hold two distant
regions at once, so a second pool was added to hold them. 81,920 B — 36% of all
RAM in use — is paid to work around an addressing scheme.

## The reclamation, staged

### Stage A — the verify scratch stops being its own allocation (8,192 B)

`s_v11_verify_scratch` has exactly two use sites, `xfer_v11_write()` and the
`'U'` bulk-sector handler. Both are transfer-mode only, and transfer mode stops
playback at both ends: the audio block returns silence
(`if (g_xfer_mode) { memset(...); return; }`) and the streamer skips
(`if (g_xfer_mode) { k_msleep(1); continue; }`). Leaving transfer mode sets
`g_slot_switch_req`, which reloads the song and re-primes the ring from empty —
so the ring's contents are discarded on the way out regardless.

The scratch can therefore share storage with a ring slot. **This needs a CI
guard**, because the failure mode if a third use site ever appears outside
transfer mode is silent corruption of live audio, which is exactly the class of
bug this codebase spends effort to make impossible.

### Stage B — CORRECTED

**The first version of this section was wrong and is kept here as the mistake
it was.** It claimed a wrap-aware read-ahead could absorb the loop ENTRY pin
and free 49,152 B. It cannot. Read-ahead is 4 sectors deep — about 28 ms of
audio — and a single worst-case fetch was *measured* at 21-23 ms under load.
Pulling five entry sectors in during the last four sectors before a wrap has no
margin at all. The entry pin is pinned **from the moment the loop is armed**
precisely so the wrap never depends on that race, and that is correct. It
stays.

The real candidate is the **EXIT pin**, and it is stale in the most literal
sense: it holds data for an operation that was removed.

`ST_LOOP_PIN_EXIT` is documented in `main.c` as *"loop_end: where every exit
seek lands"*. But the audio thread's exit handler says:

> THE TRANSPORT IS NOT TOUCHED HERE EITHER. Releasing stops future wrapping and
> nothing more: the iteration already in flight plays on through the loop end
> and into the material that follows […] **THIS REVERSES A DOCUMENTED EARLIER
> DECISION** […] The product ruling is that a release must not move the
> playhead under any circumstances.

There is no exit seek any more. A release is ordinary continuous playback
carrying on past `loop_end`, which is the case ordinary read-ahead exists to
serve. Residency depth is a property of *seek targets*, where the ring is cold —
and this is not a seek.

| | slots | bytes |
|---|---|---|
| today: ring 6 + entry 5 + exit 5 | 16 | 131,072 |
| after: ring 6 + entry 5 | 11 | 90,112 |
| **freed** | **5** | **40,960** |

**NOT YET PROVEN, and it must be before anything is deleted.** The claim rests
on the ring's prefetch actually holding `loop_end+1…` while a loop runs. If the
prefetch target is clamped or wrapped to the loop window, that material is not
in the ring and the exit pin is still earning its place. `st_stream_required_
sector()` is `song_frame / ST11_FRAMES_PER_SECTOR` — purely linear, no loop
term — which is the evidence *for*; the producer's own `next_fill()` target
selection has not been read yet, and it is the one that decides.

**Stage B is a change to the continuous-playback path**, which is otherwise
under a standing do-not-touch rule. Own commit; loop seam and transport gates
run against it; nothing folded in.

### Combined: 49,152 B (A + corrected B), against 33,570 B free today

Still enough for song-planar's +16,384 B, with 32,768 B spare rather than
40,960 B.

## The part that changes the roadmap

The roadmap asks for **an independent playhead per track**, for scrubbing. On
today's interleaved format that is not a RAM question, it is an impossibility:

| design | RAM |
|---|---|
| today, one shared head | 131,072 B |
| 4 independent heads, **v1.1 interleaved** | **524,288 B — 2× the entire SRAM** |
| 4 independent heads, **song-planar v1.2** | **131,072 B — exactly today's** |
| 4 independent heads, song-planar + Stage B | 81,920 B |

The reason is the same one that decides reverse. On v1.1 a head must fetch a
whole 8,192-byte sector to obtain the 2,048 bytes of the one stem it plays —
25% utilisation — so four divergent heads cost four full sector pools. On
song-planar each head reads only its own stem's 2,048-byte groups, so four
heads cost what one shared head costs today.

**Song-planar is therefore not a reverse-playback feature.** It is the
precondition for per-track scrub heads, and it pays for itself in RAM the
moment more than one head exists. The earlier per-stem-head report reached the
same conclusion from the bandwidth side and named de-interleaving as "the only
way to avoid the 4× penalty."

## What the other roadmap items cost

- **Universal scrubbing + speed adjustment** — control state, not buffers.
  Scrubbing is a varispeed rate applied to an existing head; `st_pitch` and
  `st_inertia` already carry that shape. Tens of bytes.
- **Multiple songs to the memory limit** — eMMC capacity, not SRAM. The STIX
  index already carries per-song geometry; more songs is more index records on
  storage. The only SRAM cost is the working record, already allocated.
- **Heads mode** — a mode flag plus per-head position, `st_stream_t` at 36
  bytes each. Under 200 B for four.
- **MIDI cue mode** — `g_midi_held[MIDI_HELD_MAX]` already exists; cue state is
  small.

None of these are buffer-sized. **The sector pools are the entire RAM
conversation**, which is why the plan above is short.

## Order of work

0. **Prove or kill the Stage B premise** — read the producer's `next_fill()`
   target selection and settle whether the ring holds `loop_end+1…` during a
   loop. Costs nothing, decides 40,960 B, and must happen before any deletion.
1. **Stage B** (40,960 B, if step 0 confirms) — remove the exit pin region.
   Touches the playback path; own commit; loop seam + transport gates must stay
   green. Taken FIRST because its failure mode is loud (a gap on loop release,
   which the seam gate already tests for) where Stage A's is silent.
2. **Stage A** (8,192 B) — the verify scratch shares a ring slot. Needs a real
   two-thread quiesce handshake, not a timing argument: entering transfer mode
   sets the flag and returns, and nothing today *proves* the audio thread has
   observed it before the first command lands. Silent audio corruption is the
   failure mode, so this one waits for the handshake.
3. **Song-planar v1.2** (+16,384 B for per-stem rings) — fits inside what
   Stage A + B free, with 32,768 B still spare.
4. Per-track heads, then the rest of the roadmap, on the freed budget.
