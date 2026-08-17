# Stem Tape transfer contract v1.1 — crash-safe A/B storage

Status: **companion-side specification and reference implementation.** No
firmware in this repository implements it yet. Every claim below is verified
against the in-repo mock device (`src/sp1/__tests__/mockSerial.ts`), never
against hardware.

## 0. What v1.1 changes and why

v1.0 kept a single STIX index. Replacing a song rewrote that index and then
wrote the validity magic; a disconnect during the magic write could leave no
valid index, and the companion concluded the library had to be reinitialized.
That is data loss and it is not acceptable.

v1.1 stores **two song regions (A/B) and two index regions (A/B)**. A
replacement is always staged into the *inactive* pair. The active song is never
touched. The guarantee is:

> At every interruption point, either the previous song remains valid or the new
> song is completely committed. An interrupted replacement never requires
> reinitialization.

Only genuinely corrupt or blank storage — *both* index records unreadable — asks
for explicit initialization, and that is reported as corrupt storage, not as an
interrupted upload.

## 1. Unchanged Tape Looper foundation

The byte-level transport is the original Tape Looper protocol, verbatim:

| cmd | byte | payload | reply |
| --- | --- | --- | --- |
| ping | `0x50` `'P'` | — | `"SP1!"` + 24-byte layout |
| read | `0x52` `'R'` | u32 LE block | `0x72` + 512 bytes |
| write | `0x57` `'W'` | u32 LE block + 512 bytes | `0x77` ack / anything else = NAK |
| flush | `0x46` `'F'` | — | `0x66` |
| exit | `0x58` `'X'` | — | `0x78` |

Entry magic `SP1XFER!`, 512-byte blocks, 115200 baud, one in-flight command at a
time. `src/sp1/__tests__/tapeLooperConformance.test.ts` and
`transcriptAudit.test.ts` assert byte-for-byte transmitted equality between the
original companion (sliced out of `firmware/web/index.html`) and
`src/sp1/protocol.ts`. Answering `SP1XFER!` authorizes **nothing**: it makes the
device readable, never writable.

## 2. Capability query `Q` → `STCP` (v1.1 layout)

Sole gate for physical mutation. Silence = stock firmware = read-only.
Field offsets are defined once in `src/sp1/stemTapeFormat.ts` (`CAPS_OFF`) and
parsed once in `src/sp1/compatibility.ts`.

Reported fields: firmware id `'STFW'`, protocol major/minor, format major/minor,
capability flags, sample rate, block size, sector bytes, required alignment,
total device blocks, **song region A/B (start, blocks)**, **index region A/B
(start, blocks)**, advisory active index/song slot, advisory active generation,
STIX version.

Required flags: four stems, stereo, 48 kHz, 24-bit, index extension, BPM +
downbeat, explicit init, **dual song slots, dual index slots, generation commit,
crash-safe replace**.

Region validation (`validateRegions`) rejects: unaligned starts or lengths,
regions that overlap each other, regions past `deviceBlocks`, zero-length
regions, unequal-purpose geometry, and a sector size that is not 16 × 512 B.
Addresses are never guessed — every block number comes from this reply.

## 3. STIX v2 index record (256 bytes, one per index region)

Offsets: `IX_OFF` in `src/sp1/stemTapeFormat.ts`.

- `magic` — `'STIX'`, **written last**, zero while uncommitted.
- `indexVersion`, `formatMajor`, `formatMinor`.
- `slotIdentity` — which index region this record belongs to; a record found in
  the wrong region is invalid (guards a mis-addressed write).
- `songSlot`, `flags.SONG_PRESENT`.
- `generationLo` / `generationHi` — 64-bit monotonic generation, ≥ 1.
- Song geometry: start block, block count, frames, sector count.
- Audio identity: sample rate, channels, bit depth, four original frame counts,
  four stem checksums, song checksum.
- Musical metadata: BPM ×256, downbeat frame.
- Title / artist, 60 bytes each.
- `crc32` — CRC-32 (IEEE 802.3) over the whole record with the magic field and
  the CRC field itself normalized to zero, so the committed and uncommitted
  images share one CRC and the magic write is the only difference between them.

## 4. Active-index selection (`src/sp1/activeIndex.ts`)

One selector, used identically by the mock device, connection, upload preflight,
post-commit confirmation and reconnect recovery. The device's advisory
`activeIndexSlot` is never trusted.

1. Parse both index records independently.
2. Reject invalid magic, CRC, version, slot identity, bounds or song metadata.
3. Exactly one valid → select it.
4. Both valid → strictly greater generation wins (tie → A).
5. Neither valid → `blank` (all zero) or `corrupt`; explicit init required.
6. A corrupt *newer* record therefore falls back to the previous generation.
7. One invalid slot **never** requires reinitialization.

## 5. Safe replacement sequence (22 steps, `StemTapeTransport.uploadSong`)

1. Re-query `Q` immediately before writing; every immutable field must equal the
   negotiated set, or nothing is written.
2-4. Read index A, read index B, run the selector.
5. Destination = inactive song slot + inactive index slot. `generation + 1`.
6. Inactive-slot capacity check. Short capacity raises
   `InsufficientStagingCapacityError` **before any write**: the active song is
   never overwritten to make a replacement fit.
7. Safety assertion: destination ≠ active song slot, destination ≠ active index
   slot.
8. Write the song into the inactive song region (per-block retry, max 3).
9-10. Read the whole region back and compare every byte.
11-12. Recompute the four stem checksums and the song checksum from the bytes
    read back.
13. Write the **uncommitted** index record (magic = 0) into the inactive index
    region.
14. Flush.
15-16. Read it back and verify every byte except the intentionally absent magic,
    plus zero padding to 512 B.
17. Write the validity magic — the last write of the sequence.
18. Flush.
19-21. Re-read **both** index records, run the selector, require the new
    generation to be selected and to match this song field by field.
22. Only now report `committed`. The previous generation stays valid as the
    rollback copy and becomes the destination of the next replacement.

Uploads therefore alternate: song A/index B, song B/index A, song A/index B, …

## 6. Outcomes

| condition | outcome | meaning |
| --- | --- | --- |
| step 22 reached | `committed` | new generation verified on the device |
| failure before step 17 | `failed` | no magic was sent; previous generation active |
| failure at or after step 17 | `unknown` | magic may have landed; reconnect decides |
| both records invalid | `corrupt` | blank or damaged storage; explicit init |

`unknown` is never terminal: `resolveOutcome()` reads both records, runs the
same selector and returns `committed` or `failed`. A valid previous generation
means the replacement simply did not commit — reinitialization is never implied.
The forbidden phrases are asserted in
`src/sp1/wording.ts` (`FORBIDDEN_INTERRUPTION_PHRASES`).

## 7. Initialization

Explicit and user-confirmed only, and legal **only** when both index records are
invalid or blank. It writes index B as explicit zeros, then a valid song-free
generation-1 record into index A (uncommitted → flush → magic → flush). No false
song entry is created and both song regions stay free for the first upload.

## 8. Audio payload (unchanged from v1.0)

48 kHz · stereo · signed 24-bit LE · four stems sharing one frame count N
(shorter stems digitally silence-padded). 8,192-byte logical sector = 16 × 512-byte
blocks, 32-byte header + 8,160-byte payload = 340 frames/sector at 24 B/frame.

## 9. Verification claims

Three independent booleans, never conflated: `simulatedVerification`,
`deviceReadbackVerification`, `physicalPlaybackVerification`. A mock run can
only ever set the first, and the UI wording for simulated runs may not borrow
device language.

## 10. Firmware handoff checklist

A firmware implementing this contract must:

1. Answer `Q` with the v1.1 `STCP` structure and the four region descriptors.
2. Expose two song regions and two index regions, aligned, non-overlapping and
   inside `deviceBlocks`.
3. Treat a record as valid only with correct magic, CRC-32, version, slot
   identity and bounds — same rules as §4.
4. Boot from the greater valid generation, falling back to the other record.
5. Never erase or relocate the non-selected pair on boot.
6. Honour `F` as a real durability barrier: an acked flush must survive power
   loss.
7. Report the same geometry across reconnects; any change aborts a transfer.

## 11. Test coverage

- `src/sp1/__tests__/tapeLooperConformance.test.ts` — transport parity.
- `src/sp1/__tests__/transcriptAudit.test.ts` — byte-identical transcripts.
- `src/sp1/__tests__/canonicalSong.test.ts` — preparation, gate, upload, delete.
- `src/sp1/__tests__/endToEndFixtures.test.ts` — four real WAVs to device bytes.
- `src/sp1/__tests__/unknownOutcome.test.ts` — interruption matrix, successive
  uploads, torn magic write, corrupt-storage classification.
