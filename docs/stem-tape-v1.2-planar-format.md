# Stem Tape storage format v1.2 — stem-planar sectors

Status: **specification, not yet implemented.** Firmware is still v1.1
(`ST11_FORMAT_MINOR 1`); the companion is still v1.1
(`stemTapeFormat.ts: FORMAT_MINOR = 1`).

## Why the layout changes

Per-track reverse needs one stem to read from a different position in the song
than the other three. In v1.1 all four stems are interleaved inside every
frame, so a diverging stem cannot be fetched without fetching the whole sector
a second time — 143% of a read engine that has 100%.

Giving each stem its own contiguous, block-aligned plane inside the sector lets
a diverging stem fetch only its own quarter. Measured on hardware (see
`stem-tape-reverse-feasibility.md`): one reversed stem at 92% CPU busy against
an 83% baseline, zero dropouts.

**Nothing else about the device changes.** Same 8192-byte sector, same 16
blocks, same 340 frames per sector, same capacity, same wire protocol, same
STIX index. Only the arrangement of bytes inside the sector payload moves.

## The sector

```
byte 0      2048      4096      6144      8192
     |---------|---------|---------|---------|
     | plane 0 | plane 1 | plane 2 | plane 3 |
     | vocal   | drums   | bass    | instrum |
     |---------|---------|---------|---------|
block 0         4         8        12        16
```

Plane *k* holds stem *k* and nothing else, occupying exactly **4 blocks**
(2048 bytes) starting at block offset `k * 4`. That alignment is the whole
point: a single plane is independently readable with one `emmc_read_blocks()`.

### Inside one plane (2048 bytes)

| offset | size | field |
|---|---|---|
| 0 | 1 | `'P'` (0x50) |
| 1 | 1 | `'L'` (0x4C) |
| 2 | 1 | stem index, 0-3 — must equal the plane's own position |
| 3 | 1 | flags, must be 0 |
| 4 | 4 | `sectorIndex`, u32 LE |
| 8 | 2040 | 340 frames × 6 bytes: L then R, each signed 24-bit LE |

`8 + 340 × 6 = 2048` exactly, and `4 × 2040 = 8160` — **the same 340 frames and
the same 8160 audio bytes per sector as v1.1**, with the same 32 bytes of total
overhead. Sector counts, capacity arithmetic and every STIX geometry field are
therefore unchanged by this migration.

### Why a per-plane header at all

A reversed stem is fetched as a plane-only read from a *different* sector than
the other three. Without a header inside the plane, that read returns bytes
with nothing to check them against — no magic, no identity, no way to tell a
correct fetch from a mis-addressed one. The 8 bytes make a plane-only read
self-validating: magic, the stem it claims to be, and the sector it came from.

### What v1.1's 32-byte sector header carried, and where it went

| v1.1 field | disposition |
|---|---|
| `magic` 'STSC' | replaced by the per-plane `'PL'` magic |
| `sectorIndex` | in every plane header |
| `firstFrame` | **dropped** — derived: `sectorIndex * 340` |
| `frameCount` | **dropped** — derived: 340, or `frames - firstFrame` on the last sector |
| `bpmQ8` | **dropped** — see below |
| `downbeatFrame` | **dropped** — see below |
| `ledReserved`, `reserved` | **dropped** — firmware-owned, never read |

`firstFrame` and `frameCount` are safe to drop because
`st_stream_validate_sector()` already computes both as *expected* values from
`sectorIndex` and the stream geometry, and compares — it never consumes the
stored ones as data.

`bpmQ8` and `downbeatFrame` are safe to drop because the STIX record is the
authoritative timing source. The sector-0 copies are read once at boot and
compared, and `main.c`'s own comment at that site says it plainly: *"cross-
checked here, once, for consistency ONLY: a mismatch is logged as a boot
diagnostic, never acted on -- the STIX record always wins."* Dropping them
loses a diagnostic, not a function.

## Reading

| reversed stems | plan | reads | blocks |
|---|---|---|---|
| none | `blk0 +16` | 1 | 16 |
| stem 3 | `blk0 +12`, `blk12 +4` at the reverse position | 2 | 16 |
| stem 0 | `blk4 +12`, `blk0 +4` at the reverse position | 2 | 16 |
| stem 1 or 2 | **not offered** — 0.4 points from the level-2 run that failed | 2 | 20 |

Only stems at a *sector end* leave the remaining three contiguous, and level 2
FAILED on hardware (742 dropouts, 99% busy), so the middle two are not
offered — the cheapest middle-stem plan is 0.4 points away from that run.

**The plane order therefore decides which two tracks get reverse**, and it is a
v1.2 choice rather than an inherited constraint: each plane header carries its
own stem id, so any permutation of the four stems across the four plane
positions is self-describing and checkable. The two stems placed at plane 0 and
plane 3 are the reversible pair. **Pending decision — the order below is
v1.1's and is a placeholder until that is made.**

## Versioning, and why old songs cannot be misread

`ST11_FORMAT_MINOR` / `FORMAT_MINOR` go **1 → 2** on both sides. Two
independent gates already exist and both require exact equality, so no new
mechanism is needed:

- **Firmware refuses old songs.** `st_stix.c:159-160` rejects any index record
  whose `format_major`/`format_minor` do not match the firmware's own. A v1.1
  song therefore fails to load on v1.2 firmware rather than being replayed as
  though its interleaved bytes were planes.
- **Companion refuses old firmware.** `compatibility.ts` requires
  `formatMajor === FORMAT_MAJOR && formatMinor === FORMAT_MINOR` from the STCP
  capability reply, so a v1.2 companion will not upload planar data to v1.1
  firmware, and a v1.1 companion will not upload interleaved data to v1.2
  firmware.

Both directions are already fail-closed. Existing songs must be re-uploaded,
which is expected during development.

## Companion changes

Confirmed against the current source, so the blast radius is known:

**`sector.ts` — `encodeSector()` and `decodeSectors()` only.** The payload fill
loop changes from writing `dst = 32 + f*24, dst + s*6` to writing
`dst = s*2048 + 8 + f*6`, plus the four plane headers instead of one sector
header. `encodeSong()`, `sectorToBlocks()`, `blocksToSector()` and every caller
are untouched.

**`stemTapeFormat.ts`** — `FORMAT_MINOR = 2`, plus the plane constants; the
`SECTOR_OFF` table is replaced by a plane-relative one.

**Nothing else.** Two facts from the source review make this true:

1. **Checksums are layout-independent.** Each per-stem FNV-1a checksum is taken
   over that stem's own contiguous `pcm24` buffer in playback order
   (`song.ts:210`), never over assembled sector bytes; the song checksum is over
   the 16-byte digest of those four. Reordering bytes inside a sector changes
   neither. The device-side verification at `transport.ts:702-713` re-derives
   them through `decodeSectors()`, so it stays valid once that function is
   updated — and it is exactly the test that will catch a wrong reorder.
2. **The wire protocol does not move.** Sectors are always materialised as
   complete 8192-byte buffers before transmission (`encodeSong()` up front,
   `writeSectorBulk()` hard-rejecting any payload that is not exactly 8192
   bytes), so nothing is assembled incrementally and the `'U'` bulk command,
   its 17-byte header and its 14-byte response are unaffected.

## Fixtures

`handoff/v1.1/binaries/song-sectors-four-stem.bin` (43 sectors, 352,256 bytes,
songChecksum 3509299530) is consumed by twelve firmware tests and the CI
workflow. It is **kept**, and gains a second job: proving a v1.1 song is
*refused* rather than misread.

A v1.2 twin is generated from the same four source WAVs, so the two differ only
in byte arrangement. The migration's strongest available test follows from
that: decoding the v1.2 fixture must reproduce, byte for byte, the same four
per-stem PCM streams as decoding the v1.1 fixture, and therefore the same four
stem checksums and the same song checksum — 3509299530.
