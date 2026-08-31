# Stem Tape storage format v1.2 — song-planar stem streams

Status: **implemented in firmware; the companion has not caught up.** The read
path is planar and `ST11_FORMAT_MINOR` is 2; the companion still emits v1.1
(`stemTapeFormat.ts: FORMAT_MINOR = 1`), so the device currently refuses every
song it can be sent. That is the intended intermediate state — the version
gates refuse rather than misread — and step 2 closes it.

**The evidence that the audio survived the change**, before any of it reaches
hardware: the full two-thread playback gate runs the planar read path over a
v1.2 image derived from the recorded v1.1 song and produces deterministic hash
`0xe9650dda`, the same value the v1.1 path produced and the same value the
single-threaded walk established independently. CI asserts that literal, so an
edit that changes the decoded audio fails the step rather than printing a
different number.

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
FOUR things pull against each other, and the fourth was missed on the first
pass through this table:

- **RAM** = `4 stems x G x 2048`.
- **CPU** = one read per stem per refill: `4 x read(4R) / R` per span.
- **DEPTH** = `G - R` spans always buffered. Hard floor: a single worst-case
  fetch was MEASURED at 21.6-23.6 ms under load and a span is 7.083 ms, so
  **fewer than 4 spans buffered is thinner than one observed stall**.
- **R MUST DIVIDE G.** See below. This is not a tidiness preference; it
  decides whether the CPU column above is true at all.

### The rule this table originally got wrong

`emmc_read_blocks()` reads consecutive blocks into **one contiguous buffer**.
A batch of R groups is therefore one read only if those R groups occupy R
contiguous slots -- and slot is `group % G`, so a run that crosses the end of
the ring does not. When `R | G`, runs aligned to a multiple of R never cross
it and every refill is exactly one read. When it does not divide, refills wrap
and split.

G=7/R=3 is the case that exposed it. Batch sizes cycle **3, 3, 1**: one refill
in three pays a second fixed read cost for a single group.

| | modelled | actual |
|---|---:|---:|
| G=7/R=3, us/span | 3397 | **3647** |
| G=7/R=3, busy | 86% | **89.4%** |

The first version of this table quoted 3397/86% for G=7/R=3, and a ring-size
decision was made twice on that number. It assumed a clean `read(4R)/R` for
every pair, which only holds when `R | G`.

| G | R | R divides G | RAM | vs today | depth | us/span | busy |
|---|---|---|---|---|---|---|---|
| 6 | 3 | yes | 49,152 | +0 | **3 spans — too thin** | 3397 | 86% |
| **6** | **2** | **yes** | **49,152** | **+0** | **4 spans** | **3834** | **92%** |
| 7 | 3 | **NO — wraps** | 57,344 | +8,192 | 4 spans | 3647 | 89.4% |
| 7 | 1 | yes | 57,344 | +8,192 | 6 spans | 5147 | 110% |
| 8 | 4 | yes | 65,536 | +16,384 | 4 spans | 3178 | 83% |
| 5 | 1 | yes | 40,960 | −8,192 | 4 spans | 5147 | 110% |

Applying all four constraints at once — `R | G`, depth ≥ 4, and free RAM ≥ the
32,768 B floor (which caps G at 7) — leaves **exactly one viable pair**.

## DECIDED: G=6, R=2

- **RAM-neutral.** `4 × 6 × 2048 = 49,152` is byte-for-byte today's ring. The
  read path is reshaped from `[6][8192]` to `[4][6][2048]`, not grown.
- **Aligned batches.** 2 divides 6, so every refill is exactly 2 groups, 8
  blocks, one read. No split-batch handling anywhere.
- **Depth 4 spans**, unchanged.
- **The mailbox and the pins are untouched.** `ST_STEM_MBOX_SLOTS` is already
  `ST_LAT_RING_SLOTS` = 6, so even the ring constant does not move — four
  instances of the same struct, each still one index computation, still
  wait-free.

The cost is **92% busy against today's 83%**. That is the highest of the three
candidates, and it is an operating point the SP-1 has already run: the level-1
reverse test measured 92% busy with **zero silence frames**. It is a measured
operating point, not an extrapolation.

**What this buys back:** the 9,088 B Stage A reclaimed is no longer spent on
the read path at all. It stays banked for per-track scrub heads, multi-song,
heads mode and MIDI cue — the roadmap this document's RAM analysis was written
for.

If 92% proves uncomfortable on hardware, the ladder up is G=8/R=4 at 83%,
which needs the unified associative cache to fit — the same trade already
declined once, and now worth 9 points rather than 3.

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

**That is the whole budget, at G=7.** 42,658 − 8,192 = 34,466, clearing the
32,768 floor by 1,698. **The unified associative cache is no longer a
prerequisite of anything** — see the decision above, and `stem-tape-ram-v1.md`
for why the ring was the wrong donor for the scratch and stays blocked on that
same rework.

Note the arithmetic is a floor, not a total: at G=8 the same 42,658 would land
at 26,274, *below* the floor, which is what forced the decision.

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
