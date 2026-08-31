# Stem Tape storage format v1.2 — song-planar stem streams

Status: **specification, not yet implemented.** Firmware is still v1.1
(`ST11_FORMAT_MINOR 1`); the companion is still v1.1
(`stemTapeFormat.ts: FORMAT_MINOR = 1`).

## The requirement this exists to meet

> "I should be able to reverse ANY stem as long as I am only reversing one at
> a time."

That rules out the first draft of this document, which is recorded below as
the wrong answer and why.

## The rejected draft: sector-planar

The first version kept v1.1's 8192-byte sector and split it into four
2048-byte planes, one per stem. It was chosen for minimum change: same sector,
same 340 frames, same capacity, a companion diff confined to one function.

**It cannot meet the requirement, structurally.** A stem's data sits in 4
blocks out of every 16, so a reversed stem's samples are *scattered* across the
song. Fetching them needs its own read, and — worse — removing a middle plane
splits the remaining three, so they stop being one read too:

```
blocks:   0───3    4───7    8──11   12──15
          vocal    drums    bass    instrument
reverse vocal      -> forward = 4..15   contiguous   2 reads, 16 blocks   92% MEASURED PASS
reverse instrument -> forward = 0..11   contiguous   2 reads, 16 blocks   92% MEASURED PASS
reverse drums      -> forward = 0..3 + 8..15, HOLE   2 reads, 20 blocks   ~101%
reverse bass       -> forward = 0..7 + 12..15, HOLE  2 reads, 20 blocks   ~101%
```

Level 2 measured 4491 us and FAILED with 742 dropouts at 99% busy; the
cheapest middle-stem plan is 4465 us, 0.4 points away. A sector has two ends,
so no permutation of four equal planes makes more than two stems reversible.
The layout caps the feature at two, and the requirement is four.

## The format: song-planar

Each stem's **entire timeline** is contiguous, in its own quarter of the song
region:

```
song region
|------------------|------------------|------------------|------------------|
| stem 0 timeline  | stem 1 timeline  | stem 2 timeline  | stem 3 timeline  |
|------------------|------------------|------------------|------------------|
```

A stem is now read entirely independently of the others. **Reversing a stem
changes only the address its read goes to — never the number of reads, never
the bytes moved, never the cost.** That is what makes any stem reversible, and
it is why the cost is also flat in the *number* of reversed stems.

### The unit: a 4-block stem group

| offset | size | field |
|---|---|---|
| 0 | 1 | `'P'` (0x50) |
| 1 | 1 | `'L'` (0x4C) |
| 2 | 1 | stem index, 0-3 |
| 3 | 1 | flags, must be 0 |
| 4 | 4 | `groupIndex`, u32 LE — which 340-frame span this is |
| 8 | 2040 | 340 frames × 6 bytes: L then R, each signed 24-bit LE |

`8 + 340×6 = 2048` = 4 blocks exactly. Four groups (one per stem) at the same
`groupIndex` carry the same 340 frames of the song, so **frames per group is
340 — identical to v1.1's frames per sector** and every STIX geometry field
carries over unchanged.

The header makes each group self-validating from a group-only read: magic, the
stem it claims to be, and the span it covers. That matters more here than in
the rejected draft, because *every* read is now a group read.

### Region derivation

Sub-region *k* starts at `songStartBlock + k × (songBlockCount / 4)`. No new
STIX fields; `songBlockCount` must be a multiple of `4 × 4` blocks so each
quarter is a whole number of groups.

## Why this is not more expensive — the part that decides it

Read one group per stem per span and you pay four fixed read costs where v1.1
paid one: **5147 us, worse than the level-4 failure.** So groups are read in
**batches of N spans**, which is legitimate precisely because a stem's timeline
is contiguous — N spans of one stem are `4N` consecutive blocks, one read.

Measured fit: 656.4 us fixed + 157.6 us/block.

| batch N | reads per span | us per span | vs today |
|---|---|---|---|
| 1 | 4.0 | 5147 | +1969 ❌ |
| 2 | 2.0 | 3834 | +656 (92% — the level-1 number, measured clean) |
| **4** | **1.0** | **3178** | **+0 — exactly today's cost** ✅ |
| 8 | 0.5 | 2850 | −328 (cheaper than today) |

At N=4 the device moves the same 16 blocks per span and pays the same single
fixed read cost per span that it pays now — because the same bytes are being
fetched, just grouped by stem instead of by time. **Any stem reversed, all four
reversed, none reversed: 3178 us either way.**

## Buffering: ring size, refill size, and the depth that must not shrink

The read path needs a per-stem ring of G groups, refilled R groups at a time.
Three things pull against each other and only certain (G, R) pairs are safe:

- **RAM** = `4 stems x G x 2048`.
- **CPU** = one read per stem per refill: `4 x read(4R) / R` per span.
- **DEPTH** = `G - R` spans always buffered. This is the one with a hard
  floor: a single worst-case fetch was MEASURED at 21.6-23.6 ms under load, and
  a span is 7.083 ms, so **fewer than 4 spans buffered is thinner than one
  observed stall**.

| G | R | RAM | vs today | depth | us/span | busy |
|---|---|---|---|---|---|---|
| 6 | 3 | 49,152 | +0 | **3 spans — too thin** | 3397 | 86% |
| 6 | 2 | 49,152 | +0 | 4 spans | 3834 | 92% |
| **7** | **3** | 57,344 | **+8,192** | 4 spans | 3397 | **86%** |
| **8** | **4** | 65,536 | **+16,384** | 4 spans | 3178 | **83% — today's** |

G=6/R=3 is the tempting one because it is RAM-neutral, and it is **not safe**:
three spans buffered is less than one measured worst-case fetch.

**DECIDED: G=8, R=4.** 16 KB of reclamation, and ordinary playback stays at
exactly today's 83% busy — so step 3 verifies that nothing changed, rather than
signing off a regression that was chosen for convenience. The alternative
(G=7, +8 KB, 86%) was rejected because "no dropouts ever" is the standing
requirement and 3 points of ordinary-playback headroom is not the thing to
spend to save 8 KB.

Both figures are inside what `stem-tape-ram-v1.md` identifies as available:
8,192 B from the verify scratch and 8,192 B from the unified associative
cache, 16,384 B in total. **The RAM work is therefore no longer speculative
groundwork — it is a prerequisite of the read path**, which is a different
thing from the earlier framing where nothing depended on it.

G and R are compile-time constants so this is one line to revisit once the
reclamation lands.

## The cost that is real: RAM

Sector pools today, computed from the compiled latency constants
(`ST_LAT_READAHEAD_SECTORS 4`, `RESIDENCY 5`, `RING_SLOTS 6`):

| pool | size |
|---|---|
| `g_stem_sector_bufs[6][8192]` | 49,152 B |
| `g_stem_loop_pin_bufs[10][8192]` | 81,920 B |
| ~~`s_v11_verify_scratch[8192]`~~ | ~~8,192 B~~ — **reclaimed in `st38`** |
| **total** | **131,072 B** |

That last line was 139,264 B until `st38`: the upload verify scratch no longer
has an allocation of its own, it is the last loop-pin buffer. Measured on the
linked ELF, RAM used went 228,574 → **219,486** and free 33,570 → **42,658** —
9,088 B, about 900 more than the array itself, because its padding went too.

**Half the 16,384 is banked. The other half is still required**, and the
arithmetic is a floor, not a total: 42,658 − 16,384 = 26,274, which is *below*
the 32,768 CI floor by 6,494. Adding the unified associative cache's 8,192
lands at 34,466, clearing it by 1,698. So the cache rework stays a
prerequisite — see `stem-tape-ram-v1.md`, which also records why the ring was
the wrong donor for the scratch and remains blocked on that same rework.

Per-stem rings at the same 8-span depth cost `8 × 2048 × 4 = 65,536 B` against
today's 49,152 B ring — **+16,384 B**, which does not fit as things stand.

It is very likely findable rather than blocking: `g_stem_loop_pin_bufs` is
81,920 B and exists, by its own comment in `main.c`, only "because the
read-ahead ring maps sector s to slot s % SLOTS, so a window wider than the
ring cannot hold both of its ends — an artefact of the ring's addressing, not a
property of the problem. A unified cache with associative lookup and pinnable
slots removes them entirely." Song-planar rings want exactly that rework
anyway. `s_v11_verify_scratch` was another 8,192 B used only while playback is
paused for upload — **that one is now reclaimed** (`st38`).

**But it is not measured, and nothing here should be read as though it were.**
The RAM reclamation is a prerequisite, not a footnote.

## Versioning

Unchanged from the rejected draft, and still free: `ST11_FORMAT_MINOR` /
`FORMAT_MINOR` go 1 → 2, and both existing gates require exact equality —
`st_stix.c:159-160` rejects an index record whose format version differs from
the firmware's, and `compatibility.ts` requires the STCP reply to match the
companion's. Old songs are refused rather than misread, in both directions.
Existing songs must be re-uploaded.

## Companion changes

Larger than the rejected draft's one function, but still contained, and the two
facts that bounded it still hold:

1. **Checksums are layout-independent** — each per-stem FNV-1a is over that
   stem's own contiguous `pcm24` in playback order (`song.ts:210`), never over
   assembled sector bytes. Song-planar changes neither the per-stem checksums
   nor the song checksum.
2. **The wire protocol does not move** — `writeSectorBulk()` sends a complete
   8192-byte buffer to a destination block. Song-planar changes what those
   8192 bytes contain (four consecutive groups of one stem) and which block
   they go to, not the `'U'` command, its 17-byte header or its 14-byte reply.

`encodeSong()` changes shape: instead of `ceil(frames/340)` mixed sectors it
emits four streams of groups, each written into its own quarter.
`decodeSectors()` inverts it. `sectorToBlocks()`, the transport state machine,
the index builder and the capability gate are untouched.

## Fixtures

`handoff/v1.1/binaries/song-sectors-four-stem.bin` (43 sectors, 352,256 bytes,
songChecksum 3509299530) is consumed by twelve firmware tests and CI. It is
kept, and gains a second job: proving a v1.1 song is *refused*.

The v1.2 twin is generated from the same four source WAVs, so the two differ
only in byte arrangement — which gives the migration's strongest test:
decoding the v1.2 fixture must reproduce, byte for byte, the same four per-stem
PCM streams as decoding the v1.1 fixture, and therefore the same song checksum,
3509299530.
