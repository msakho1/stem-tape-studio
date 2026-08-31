# v1.2 song-planar — the companion change, as a handoff to Lovable

The firmware read path is already v1.2 and already refuses v1.1 songs, so the
companion is currently unable to upload anything the SP-1 will accept. This
document is the complete specification for closing that, written to be pasted
to Lovable with no other context.

Everything here is derived from the firmware implementation that is already
merged and CI-green (`firmware/stemtape_player/src/st_planar.{h,c}`), not from
a plan. Where this document and that code disagree, the code is right.

---

## PROMPT FOR LOVABLE — copy everything below this line

The SP-1 firmware storage format has changed from v1.1 to v1.2. The firmware
side is done and already rejects v1.1 songs, so the companion must be updated
before any upload will be accepted. Please implement the following.

### What changed, in one sentence

v1.1 interleaved all four stems into every audio frame. v1.2 stores each
stem's **entire timeline contiguously**, in its own quarter of the song region.

The reason: the SP-1 needs to play one stem backwards while the other three
play forwards. Under v1.1 fetching one stem's divergent position costs a whole
8192-byte sector to get 2048 useful bytes, which was measured on hardware and
does not fit. Under v1.2 a stem is fetched entirely on its own, so where it
reads from changes only the *address* of the read — never the number of reads,
never the bytes moved, never the cost.

### The unit: a 2048-byte "group"

A group holds **one stem's** 340 frames. Exactly 4 physical blocks.

| offset | size | field |
|---|---|---|
| 0 | 1 | `0x50` (`'P'`) |
| 1 | 1 | `0x4C` (`'L'`) |
| 2 | 1 | stem index, 0–3 |
| 3 | 1 | flags — **must be 0** |
| 4 | 4 | `groupIndex`, uint32 little-endian |
| 8 | 2040 | 340 frames × 6 bytes |

Each frame is 6 bytes: **L then R, each a signed 24-bit little-endian sample.**

`8 + 340 × 6 = 2048`. The header exists so a group-only read is
self-validating — under v1.2 every read is a group read, and the firmware
rejects any group whose magic, stem index or groupIndex is not exactly what it
asked for.

**Stem order is unchanged from v1.1: 0 = vocal, 1 = drums, 2 = bass,
3 = instrument.**

### Where each group goes

Let `groups` = the number of groups per stem = **exactly v1.1's `sectorCount`**
(frames per group is 340, the same as v1.1's frames per sector, so this number
does not change).

```
blockOf(stem, groupIndex) = songStartBlock + (stem * groups + groupIndex) * 4
```

So the song region is four equal quarters laid out stem-major:

```
| stem 0 timeline | stem 1 timeline | stem 2 timeline | stem 3 timeline |
```

A song occupies **exactly the same number of blocks it did under v1.1**
(`groups * 16`), so every STIX geometry field — `frames`, `sectorCount`,
`songBlockCount`, `songStartBlock` — carries over completely unchanged.

### Partial final group

If the song's frame count is not a multiple of 340, the last group of each stem
is partially filled. **Zero-pad the remainder** — write silence, not stale
bytes. The firmware pads the same way and a test asserts it.

### Version

`stemTapeFormat.ts`: `FORMAT_MINOR = 1` → `FORMAT_MINOR = 2`.

This must ship in the same change as the encoder. The firmware rejects any
index record whose format version differs from its own, and `compatibility.ts`
requires the STCP reply to match the companion's — that mutual refusal is
deliberate, because a v1.1 sector read as four planar groups is not noise, it
is one stem's timeline played as all four stems. It would sound like a broken
mix rather than like an error.

### What does NOT change — please do not touch these

1. **Checksums.** Each per-stem FNV-1a is computed over that stem's own
   contiguous `pcm24` in playback order (`song.ts:210`), never over assembled
   sector bytes. Song-planar changes neither the per-stem checksums nor the
   song checksum. If a checksum moves, something is wrong.
2. **The wire protocol.** `writeSectorBulk()` still sends a complete 8192-byte
   buffer to a destination block. What changes is what those 8192 bytes
   contain (four consecutive groups **of one stem**) and which block they go
   to. The `'U'` command, its 17-byte header and its 14-byte reply are all
   untouched.
3. `sectorToBlocks()`, the transport state machine, the index builder and the
   capability gate.

### The functions to change

- `encodeSong()` — instead of `ceil(frames / 340)` mixed sectors, emit four
  streams of groups, each written into its own quarter.
- `decodeSectors()` — invert it.

### The acceptance test — please run this and report the numbers

Encode the existing four-stem reference song and confirm these are **unchanged
from v1.1**:

| | expected |
|---|---:|
| vocal checksum | 1982348978 |
| drums checksum | 207735031 |
| bass checksum | 3388280807 |
| instrument checksum | 3473776285 |
| song checksum | 3509299530 |
| total size | 352256 bytes (43 groups per stem) |

Then round-trip: `decodeSectors(encodeSong(x))` must reproduce all four stems'
PCM **byte for byte**. The firmware has an equivalent test that passes, so if
these disagree, the two implementations of the same format have diverged and I
need to see the actual bytes rather than a summary.

### What to send back

The changed files (`sector.ts`, `stemTapeFormat.ts`, and anything else you had
to touch), plus the five checksum values your encoder actually produced. I will
verify your encoder against the firmware's own `st_planar` implementation and
against the recorded reference song before anything is uploaded to hardware.

## END OF PROMPT

---

## What happens when Lovable's answer comes back

1. Diff the returned encoder against `st_planar.c`'s `st_pl_group_block()` and
   `st_pl_write_header()` — the two implementations must agree on addressing
   and header bytes exactly.
2. Generate a v1.2 fixture from the companion and check it byte-for-byte
   against the one the firmware derives from the v1.1 recording via
   `st_pl_from_v11_sector()`. Those two paths are independent, so agreement is
   real evidence rather than a shared assumption.
3. Only then is step 3 (verify ordinary four-stem playback on hardware)
   meaningful, because only then can the device be given a song it accepts.
