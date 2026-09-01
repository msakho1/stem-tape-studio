# v1.3 companion: the one thing still outstanding

Addendum to `docs/stem-tape-v1.3-companion-lovable-prompt.md`. Everything the
report described is right and nothing in the contract has moved. What is left
is **proof of layout**, which the checksums structurally cannot give.

## Why checksums are not enough

The per-stem checksums are folded over samples in timeline order. They are
therefore **identical for the interleaved and the planar arrangement of the
same song** — that is deliberate (it is what let the v1.2→v1.3 migration move
bytes without recomputing anything), and it means four matching checksums do
not prove a single byte is in the right place. A song written with the stems
transposed, or with the group headers missing, or with the final group padded
at the wrong length, checksums correctly and then plays as noise.

Only a byte-level comparison of the assembled region closes that. Below are
the exact bytes the firmware expects, computed from the frozen reference song
by the production code itself, so this can be checked without a round trip.

## The reference song

`handoff/v1.3/binaries/song-sectors-four-stem.bin` — 14,592 frames, 48 kHz,
stereo, four stems.

| Quantity | Value |
|---|---|
| Frames | 14,592 |
| Frames per group | 510 |
| Groups per stem | 29 (`ceil(14592 / 510)`) |
| Real frames in the last group | 312 |
| Zero-padded frames in the last group | 198 |
| Blocks per group | 4 |
| Total blocks | 464 (`29 × 4 stems × 4`) |
| Total bytes | 237,568 |

The 29 / 464 the report quoted match exactly.

## The five checksums

FNV-1a (`h = 0x811c9dc5`, prime `0x01000193`) over the **stored 16-bit image**,
whole groups including the zero padding.

| Stem | Decimal | Hex |
|---|---|---|
| 0 vocal | 2642900572 | `0x9d87765c` |
| 1 drums | 4238229877 | `0xfc9e4175` |
| 2 bass | 3150049925 | `0xbbc1f285` |
| 3 instrument | 962109097 | `0x39589ea9` |

The **song checksum** is FNV-1a over the 16-byte digest formed by writing those
four as little-endian `u32` in stem order:

```
digest        5c 76 87 9d  75 41 9e fc  85 f2 c1 bb  a9 9e 58 39
song checksum 1705774304   0x65ac0ce0
```

Verified with the firmware's own `st_checksum32_compute()`, not a
reimplementation.

## The group header, and the byte the report did not confirm

Each 2048-byte group is an 8-byte header then 510 frames of 4 bytes:

| Offset | Size | Value |
|---|---|---|
| 0 | 1 | `0x50` `'P'` |
| 1 | 1 | `0x4C` `'L'` |
| 2 | 1 | stem index, 0–3 |
| 3 | 1 | **`0x03`** — `ST_PL_FORMAT_V13` |
| 4 | 4 | group index, `u32` LE |
| 8 | 2040 | audio: L then R, signed 16-bit LE |

Byte 3 is the one to check. The report showed `GROUP_FLAGS_V13 = 3` as a
defined constant; what matters is that it is **written into every group**, not
just defined. `st_pl_read_header()` runs on every fetch and a group carrying
anything else is refused mid-playback, per stem, as a fetch failure rather than
as a version error — so getting this wrong looks like a streaming bug, not a
format one.

Addressing is song-planar: each stem's whole timeline is contiguous in its own
quarter of the region.

```
block(stem, g) = song_start_block + (stem * groups_per_stem + g) * 4
```

## Expected first 32 bytes of each stem's first group

Produced by running the shipped `st_pl_from_v11_sector()` over the frozen
fixture's first sector. These are the literal bytes at the start of each
stem's region.

```
stem 0   50 4c 00 03 00 00 00 00  70 c1 42 c9 e0 c1 dc c9
         50 c2 76 ca c0 c2 10 cb  30 c3 aa cb a0 c3 44 cc

stem 1   50 4c 01 03 00 00 00 00  14 d1 e6 d8 a3 d1 82 d9
         33 d2 1e da c2 d2 bb da  51 d3 57 db e1 d3 f3 db

stem 2   50 4c 02 03 00 00 00 00  cc b1 cc b1 48 b3 48 b3
         c4 b4 c4 b4 40 b6 40 b6  bc b7 bc b7 38 b9 38 b9

stem 3   50 4c 03 03 00 00 00 00  b8 e0 b8 e0 61 e1 61 e1
         0a e2 0a e2 b3 e2 b3 e2  5c e3 5c e3 05 e4 05 e4
```

Stems 2 and 3 have identical L and R in this passage; that is the reference
material, not a bug.

## What to send back

1. The same four 32-byte runs from your assembled region for this song. If
   they match, the layout is proven and nothing else is needed.
2. The complete **256-byte index record** as hex for one committed upload, so
   it can go through the real `st_stix_parse()` and its CRC-32 check. The CRC
   is over bytes `[0, 252)` with `[0, 4)` zeroed.
3. Confirmation of three behaviours the report did not cover:
   - the partial final group is zero-padded to a full 510 frames (312 real +
     198 zero here) — the checksums above include that padding;
   - groups are uploaded in ascending `seq` with no gaps;
   - an `unknown` transfer outcome never leaves the device with a damaged
     song, only with an unconfirmed one.

## What has not changed

Nothing in the wire protocol, the storage layout, the checksums or the
negotiation. The firmware moved to build tag `st54` since the prompt was
written, but that change is entirely on the LED metering side and is invisible
to the companion.
