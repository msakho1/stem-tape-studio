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

## Why the loop-pin pool exists (and why it survived scrutiny)

From its own comment in `main.c`:

> …the read-ahead ring maps sector s to slot s % SLOTS, so a window wider than
> the ring cannot hold both of its ends — an artefact of the ring's addressing,
> not a property of the problem. A unified cache with associative lookup and
> pinnable slots removes them entirely.

A modulo-addressed ring cannot hold two distant regions at once, so a second
pool was added to hold them. 81,920 B — 36% of all RAM in use — is paid to work
around an addressing scheme.

**That reads like the obvious target and it is not one.** Both regions turned
out to be load-bearing; see Stage B below for the two attempts and why each
failed. Replacing the addressing with an associative cache is still the right
change for its own sake, but it buys one slot, not a pool.

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

### Stage B — CORRECTED TWICE. Neither pin region is reclaimable.

This section has now been wrong twice, and both versions are recorded because
the reasoning matters more than the conclusion.

**First claim (wrong): a wrap-aware read-ahead absorbs the ENTRY pin, −49,152
B.** It cannot. Read-ahead runs ~28 ms ahead and a single worst-case fetch
measured 21-23 ms under load, so pulling five entry sectors in during the last
four before a wrap has no margin. The entry pin is held from the arm precisely
so the wrap never depends on that race.

**Second claim (also wrong): the EXIT pin is stale, −40,960 B.** Its label —
*"loop_end: where every exit seek lands"* — IS stale wording: the exit seek was
removed, and a release now plays forward through `loop_end` without moving the
playhead. But the region is not stale, and `main.c`'s prefetch comment says why:

> READ-AHEAD STAYS IN SONG ORDER, EVEN INSIDE A LOOP […] sector s and sector
> s+SLOTS map to the SAME slot. Inside a loop both of those are live sectors, so
> prefetching the post-wrap region evicts the pre-wrap region the playhead has
> not reached yet. […] The wrap is covered by the PINNED sectors instead,
> **exactly as the exit is**; the pin lives outside the ring and so cannot
> collide with anything.

The ring's slots cycle with the loop, so whatever it once held past `loop_end`
is evicted by later iterations. A release can arrive at any point in any
iteration and must continue past `loop_end` with no gap — which is exactly what
the exit pin guarantees and the ring cannot. **It stays.**

The only correct thing to do with this finding is fix the misleading comment,
not the allocation.

### NO LONGER OPTIONAL

The planar read path needs G=8 groups per stem, which is +16,384 B, and the
buffered-depth floor rules out every cheaper ring (see
`stem-tape-v1.2-planar-format.md`). Both reclamations below are therefore
prerequisites of the read path rather than the speculative groundwork this
document originally framed them as.

### What is actually reclaimable

| | bytes | status |
|---|---|---|
| Stage A — verify scratch shares a ring slot | 8,192 | real, needs a quiesce handshake |
| unified associative cache (16 pools → 15 live) | 8,192 | real but marginal; large change for one slot |
| entry pin | 0 | load-bearing |
| exit pin | 0 | load-bearing |

**Realistic total: 8,192 B, or 16,384 B if the cache rework is also done.** Not
the 57,344 B this document first claimed.

## The part that changes the roadmap

The roadmap asks for **an independent playhead per track**, for scrubbing. On
today's interleaved format that is not a RAM question, it is an impossibility:

| design | RAM |
|---|---|
| today, one shared head | 131,072 B |
| 4 independent heads, **v1.1 interleaved** | **524,288 B — 2× the entire SRAM** |
| 4 independent heads, **song-planar v1.2** | **131,072 B — exactly today's** |
| 4 independent heads, song-planar | 131,072 B |

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

1. **Song-planar v1.2 — RAM-NEUTRAL, and therefore not blocked.** At equal
   read-ahead depth it holds the same audio regrouped: 16 spans × 2048 B × 4
   stems = 131,072 B, exactly today's 16 sectors × 8192 B. The +16,384 B this
   document first quoted assumed a deeper ring (18 spans) for comfortable
   4-span batching. That is a nicety, not a requirement, and 33,570 B is free
   today. **The RAM work is not a prerequisite for the format change.**
2. **Fix the stale `ST_LOOP_PIN_EXIT` comment** — it describes a seek that no
   longer exists and sent this analysis down a blind alley twice. Zero risk.
3. **Stage A** (8,192 B) — the verify scratch shares a ring slot, once a real
   two-thread quiesce handshake exists. Not a timing argument: entering
   transfer mode sets the flag and returns, and nothing today *proves* the
   audio thread observed it before the first command lands. Silent audio
   corruption is the failure mode.
4. **Unified associative cache** (8,192 B) — worth doing for the addressing,
   marginal for the RAM. Not urgent while song-planar is neutral.
4. Per-track heads, then the rest of the roadmap, on the freed budget.
