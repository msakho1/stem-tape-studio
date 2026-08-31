# Stem Tape RAM — where it goes, and how to get it back

`main.c` has referenced this document for some time; it did not exist. Written
now because the roadmap (per-track scrub heads, multi-song, heads mode, MIDI
cue) needs headroom that does not currently exist, and because song-planar
v1.2 needed RAM it did not have.

Every number below is computed from the compiled constants
(`ST_LAT_READAHEAD_SECTORS 4`, `ST_LAT_RESIDENCY_SECTORS 5`,
`ST_LAT_RING_SLOTS 6`), not estimated.

**Status: Stage A is done (`st38`), and it was the only stage needed.** The
planar read path was re-sized to G=7/R=3 and now fits inside what Stage A
returned. The figures below are as they stood when this analysis started;
"What Stage A actually returned" has the measured after.

## Where the RAM actually is

228,574 B used of 262,144 B. **Three arrays are 139,264 B of it — 61%.**

| pool | slots | bytes | purpose |
|---|---|---|---|
| `g_stem_sector_bufs` | 6 × 8192 | 49,152 | read-ahead ring |
| `g_stem_loop_pin_bufs` | 10 × 8192 | 81,920 | loop entry + exit residency |
| `s_v11_verify_scratch` | 1 × 8192 | 8,192 | upload read-back verify — **since reclaimed** |
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

### Stage A — the verify scratch stops being its own allocation — **DONE, `st38`, 9,088 B measured**

`s_v11_verify_scratch` has exactly two use sites, `xfer_v11_write()` and the
`'U'` bulk-sector handler. Both are transfer-mode only, and transfer mode stops
playback at both ends: the audio block returns silence
(`if (g_xfer_mode) { memset(...); return; }`) and the streamer skips
(`if (g_xfer_mode) { k_msleep(1); continue; }`). Leaving transfer mode sets
`g_slot_switch_req`, which reloads the song and re-primes the ring from empty —
so the ring's contents are discarded on the way out regardless.

The scratch can therefore share storage with another sector buffer. **This
needs a CI guard**, because the failure mode if a third use site ever appears
outside transfer mode is silent corruption of live audio, which is exactly the
class of bug this codebase spends effort to make impossible.

#### But not with a *ring* slot — and that is the whole interesting part

The read-ahead ring is the obvious donor and it is the wrong one. Aliasing a
ring slot is sound on the two ends that *stop* — both threads now provably
quiesce — but not on what is **left behind**. The SPSC mailbox goes on
publishing the slot it last published, and leaving transfer mode by `'X'`
commits nothing and therefore reloads nothing: playback resumes with the ring
still claiming a slot whose bytes the upload overwrote. Nothing errors. It
just sounds wrong.

Making that claim true again needs a way to say *"nothing in this ring is
resident any more"*, and the mailbox API cannot say it — `st_stem_mbox_init()`
requires a quiescent ring **and** asserts that one named sector is *already*
resident and adopted, which is precisely what is false after a transfer has
written over the pool. That reclamation waits for the unified cache, which
carries per-slot validity and can represent "void" directly.

**The loop pins can say it.** `base = -1` *is* that state, it is already part
of the published pin protocol, and the loop's own end already uses it: *"drop
the pin so the ring alone feeds playback again and no stale sector can ever be
preferred over a fresh one."* So the scratch became the **last pin buffer**,
and the invariant closes without inventing anything:

1. Entering transfer drops both pins, so no valid claim on any pin buffer
   survives.
2. Neither thread can be mid-access — the handshake below.
3. The streamer's pin-fill step sits *below* its own `if (g_xfer_mode)` skip,
   so it cannot refill a pin during a transfer either.
4. On exit, `base(-1) != want`, so the next streamer pass refills from flash —
   the same path a re-arm takes, and playback is stopped throughout a transfer
   anyway.

The pin *depth* does not shrink. The last buffer stays a full member of its
region; only its contents are dropped across a transfer.

**The prerequisite is now built.** Everything above turns on "transfer mode
stops playback at both ends", and until now that was a *timing* claim, not a
proven one: entering transfer mode raised `g_xfer_mode` and returned, with
nothing establishing that either thread had observed it before the first
command started writing. `st38` replaces the assumption with a handshake —
each thread sets an acknowledgement bit at the exact point it provably stops
touching stem buffers (the audio thread where it silences the block, the
streamer where it skips its pass), both bits are cleared on the way in so a
stale one cannot stand in for a fresh one, and **no command dispatches at all**
until both are set. Gating the whole dispatch rather than the commands that
happen to touch shared storage is deliberate: a future command would otherwise
have to remember to opt in, and forgetting is silent. The gate carries a 1000 ms
escape back to ordinary playback, because while gated no command is consumed and
the ordinary idle timeout is therefore unreachable.

Two CI scripts, not one: `stemtape_player_xfer_quiesce_gate.py` proves the
handshake is wired, and `..._gate_selftest.py` proves that gate is not vacuous
by breaking each of its checks in turn — thirteen mutants, including a gate
that is entirely correct but sits *after* the command byte is consumed, and one
that aliases the ring instead of a pin — and requiring a rejection for each.

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

### STAGE A WAS THE ONLY ONE NEEDED

The planar read path needs +8,192 B at the adopted G=7/R=3, and Stage A
returned 9,088. **The cache rework is not a prerequisite of anything** — see
`stem-tape-v1.2-planar-format.md` for the decision and why it reversed an
earlier G=8/R=4 choice.

### What is actually reclaimable

| | bytes | status |
|---|---|---|
| Stage A — verify scratch shares a **pin** buffer | **9,088 measured** | **DONE, `st38`** |
| unified associative cache (16 pools → 15 live) | 8,192 | **deliberately not taken** — see below |
| entry pin | 0 | load-bearing |
| exit pin | 0 | load-bearing |

**Realistic total: 9,088 B taken, 8,192 B left on the table on purpose.** Not
the 57,344 B this document first claimed.

The cache rework is the last 8,192, and it is declined rather than pending.
Its cost is not the code churn, it is what the code does: the SPSC mailbox
keeps the audio thread **wait-free** because sector `s` always lives in slot
`s % SLOTS`, so residency is one index computation and one atomic load, never
a scan. An associative cache makes the audio thread search. Spending that to
buy 3 points of ordinary-playback headroom means rewriting the primitive that
guarantees no dropouts in order to reduce the chance of dropouts, which is the
wrong direction. It stays available if 86% proves uncomfortable on hardware.

### What Stage A actually returned — measured from the linked ELF

| | before | after (`st38`) |
|---|---:|---:|
| RAM used | 228,574 | **219,486** |
| RAM free of 262,144 | 33,570 | **42,658** |

**9,088 B, not the 8,192 predicted** — about 900 bytes more than the array
itself, because the padding around it went with it. Recorded as measured
rather than as intended; every other figure in this document should be read
the same way.

**And the arithmetic is a floor, not a total — worth doing rather than
eyeballing.** 42,658 free looks like plenty against a 16,384 ask, but the gate
constrains what *remains*:

| | free |
|---|---:|
| now | 42,658 |
| at G=8/R=4 (+16,384) | 26,274 — **below the 32,768 floor by 6,494** |
| **at G=7/R=3 (+8,192)** | **34,466 — clears it by 1,698** |

That is what forced the ring size to be re-decided. G=8 could only be afforded
by also taking the cache's 8,192, and the two together land at exactly the same
34,466 as G=7 alone — so the extra 3 points of headroom cost a rewrite of the
mailbox and bought no RAM at all. **G=7/R=3 adopted; margin 1,698 B**, thin
enough that nothing else should grow without re-checking this table.

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

Rewritten to what actually happened, rather than what was planned.

1. **Stage A — DONE (`st38`).** The verify scratch has no allocation of its
   own; it is the last loop-pin buffer, behind a real two-thread quiesce
   handshake. Not a timing argument: entering transfer mode used to set the
   flag and return, with nothing proving the audio thread had observed it
   before the first command landed. Silent audio corruption was the failure
   mode. **9,088 B measured.**
2. **Fix the stale `ST_LOOP_PIN_EXIT` comment** — it describes a seek that no
   longer exists and sent this analysis down a blind alley twice. Zero risk,
   still outstanding.
3. **Song-planar v1.2 at G=7/R=3** — +8,192 B, which Stage A already paid for.
   Ordinary playback moves from 83% to 86% busy; buffered depth is unchanged
   at 4 spans.
4. **Unified associative cache — DECLINED, not deferred.** Worth doing for the
   addressing; the RAM is marginal and the cost is that the audio thread stops
   being wait-free. Revisit only if 86% proves uncomfortable on hardware, when
   it is the thing that buys the 3 points back.
5. Per-track heads, then the rest of the roadmap, on the remaining budget.
