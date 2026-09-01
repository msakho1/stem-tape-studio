# v1.3 companion — the Lovable prompt

The SP-1 firmware is v1.3 and **refuses v1.1 and v1.2 media by design**, at
three independent layers. No existing companion build can upload anything this
firmware will accept, so the companion is rebuilt against this document.

Everything below is derived from firmware source that is merged and CI-green —
`st_v11_format.h`, `st_planar.{h,c}`, `st_stix.c`, `st_bulk_xfer.h`,
`st_ab_session.c`, and `tools/stemtape-v13-convert.py`. Where this document and
that code disagree, **the code is right**; every constant here was read out of
it rather than remembered.

Firmware reference: commit `bd8114b`, build tag `st53`,
`sha256 cb9d4a73731877ee0c7146be86a94d3e048253458d77c5d7bbd4fc3fd84eb713`.

---

## PROMPT FOR LOVABLE — copy everything below this line

Build a web companion app for the Teenage Engineering SP-1 running Stem Tape
v1.3 firmware. It connects over WebSerial, converts a four-stem song into the
device's storage format, uploads it crash-safely, and reports what happened.

Ship it in TypeScript. Keep the format code pure and unit-tested, separate from
the transport and the UI — the format is the part that must be exactly right.

---

### 1. What the app does

1. The user supplies **four audio stems**: vocal, drums, bass, instrument.
2. The app resamples/normalises them to a common length, 48 kHz, stereo.
3. It converts them to the device's v1.3 storage layout.
4. It uploads over WebSerial using a crash-safe A/B replacement sequence.
5. It reports `committed`, `failed`, `unknown` or `corrupt` — never a guess.

The device holds **one song at a time**, plus the previous one as an automatic
rollback copy. Uploading alternates between two song regions and two index
regions, so an interrupted upload never damages the song already on the device.

---

### 2. Transport basics

- WebSerial, **115200 baud**, 8N1.
- Entry magic: send the ASCII bytes `SP1XFER!` to put the device into transfer
  mode. **Answering it authorises nothing** — every command below is checked on
  its own merits.
- **One command in flight at a time.** No pipelining.
- All multi-byte fields, everywhere in this document, are **little-endian**.
- A physical block is **512 bytes**. A sector is **16 blocks = 8192 bytes**.

Commands:

| verb | byte | request | response |
|---|---|---|---|
| ping | `0x50` `'P'` | — | `"SP1!"` + 24-byte layout |
| capability | `0x51` `'Q'` | — | `"STCP"` + 96-byte record (100 total) |
| read | `0x52` `'R'` | u32 block | `0x72` + 512 bytes |
| write | `0x57` `'W'` | u32 block + 512 bytes | `0x77` = ack, anything else = NAK |
| bulk write | `0x55` `'U'` | 17-byte header + 8192 bytes | 14-byte response |
| flush | `0x46` `'F'` | — | `0x66` |
| exit | `0x58` `'X'` | — | `0x78` |

`'W'` is the **only** way to write an index record. `'U'` is the only sane way
to write song data — per-block writes would be ~509,000 round trips for a real
song against ~31,800 with `'U'`.

---

### 3. Version negotiation — fail closed, before anything is written

Send `'Q'`. The reply is `"STCP"` then a 96-byte record:

| offset | type | field |
|---|---|---|
| 0 | u32 | firmwareId — must be `0x53544657` (`'STFW'`) |
| 4 | u16 | protoMajor |
| 6 | u16 | **protoMinor — must be `3`** |
| 8 | u16 | formatMajor |
| 10 | u16 | **formatMinor — must be `3`** |
| 12 | u32 | capability flags |
| 16 | u32 | sampleRate |
| 20 | u32 | blockSize |
| 24 | u32 | sectorBytes |
| 28 | u32 | alignment |
| 32 | u32 | deviceBlocks |
| 36 | u32 | songAStart |
| 40 | u32 | songABlocks |
| 44 | u32 | songBStart |
| 48 | u32 | songBBlocks |
| 52 | u32 | indexAStart |
| 56 | u32 | indexABlocks |
| 60 | u32 | indexBStart |
| 64 | u32 | indexBBlocks |
| 68 | u32 | activeIndex — 0=A, 1=B, `0xFFFFFFFF`=none |
| 72 | u32 | activeSong — 0=A, 1=B, `0xFFFFFFFF`=none |
| 76 | u32 | activeGenerationLo |
| 80 | u32 | activeGenerationHi |
| 84 | u16 | stixVersion — must be `2` |
| 86 | 10 bytes | reserved, must be zero |

**If `protoMinor` or `formatMinor` is not 3, refuse to upload and say so
plainly.** This is mutual: the firmware refuses the companion's records too.

That refusal matters more than a normal version check. A v1.1 sector read as
four v1.3 groups is not noise — it is one stem's timeline played as all four
stems, at full scale. A v1.2 group read at 16 bits is noise, at full scale.
Neither fails safe by accident, so neither is ever attempted.

---

### 4. Audio geometry

- **48000 Hz**, **stereo**, **4 stems**, stem order fixed:
  `0 = vocal, 1 = drums, 2 = bass, 3 = instrument`.
- **Samples are signed 16-bit little-endian.** A stereo frame for one stem is
  **4 bytes**: L low byte, L high byte, R low byte, R high byte.
- All four stems are padded with silence to one common `frames` length. Keep
  each stem's true length in `originalFrames[stem]` — it is stored, not lost.

#### Converting from 24-bit sources

If your pipeline produces 24-bit (Q23) samples, convert with
**round-to-nearest, saturating, and NO dither**:

```ts
function to16(v: number): number {          // v is a signed 24-bit sample
  const q = (v + 128) >> 8;                 // round half away from -inf
  return q > 32767 ? 32767 : q < -32768 ? -32768 : q;
}
```

This is not a style preference, it is measured. Rendering the reference song
both ways through the device's own mixdown and differencing:

| method | residual RMS | peak error | DC bias |
|---|---|---|---|
| truncate (`>> 8`) | −93.3 dBFS | 2 LSB | **+0.44 LSB** |
| **round (`(v+128) >> 8`)** | **−93.5 dBFS** | **1 LSB** | none |
| TPDF dither | −89.3 dBFS | 4 LSB | none |

Truncation biases every sample toward negative infinity — a DC offset on the
whole mix. Dither is worse here and costs 4 dB: dither decorrelates a *final*
quantisation, and this one is not final, because the device re-quantises to
int16 after summing four stems and that undithered stage sets the error floor
at every level. Swept from 0 to −60 dB of source level the dithered residual
stayed flat at −89 dBFS while rounding stayed at −93. **Do not put an RNG in
the encoder.**

---

### 5. The storage unit: a 2048-byte "group"

Each stem's **entire timeline is stored contiguously**, in its own quarter of
the song region. The unit is a group: one stem, 510 frames, exactly 4 blocks.

| offset | size | field |
|---|---|---|
| 0 | 1 | `0x50` (`'P'`) |
| 1 | 1 | `0x4C` (`'L'`) |
| 2 | 1 | stem index, 0–3 |
| 3 | 1 | **flags — must be `3`** |
| 4 | 4 | `groupIndex`, u32 LE |
| 8 | 2040 | 510 frames × 4 bytes |

`8 + 510 × 4 = 2048`. Exactly.

**The flags byte is the format version and it must be 3.** It was written as 0
and never read before v1.3. It is now checked on every single group fetch,
because a song already on the card from before the migration carries a correct
`'PL'` magic, a correct stem and a correct group index — every check that
existed before this one would pass it, and the device would decode 24-bit bytes
as 16-bit ones and play the result at full volume. `0` is reserved forever as
"not this format".

The header exists so a group-only read is self-validating. Under this layout
every read is a group read, and the firmware rejects any group whose magic,
stem index or `groupIndex` is not exactly what it asked for.

#### Why the layout is this shape

The SP-1 plays one stem backwards while the other three play forwards. If the
stems were interleaved, fetching one stem's divergent position would cost a
whole 8192-byte sector to get 2048 useful bytes — measured on hardware, and it
does not fit. Contiguous per stem, a stem is fetched entirely on its own, so
where it reads from changes only the *address* of the read. Never the number of
reads, never the bytes moved, never the cost.

---

### 6. Addressing

```
groups            = ceil(frames / 510)          // groups per stem
songBlockCount    = groups * 16
blockOf(stem, g)  = songStartBlock + (stem * groups + g) * 4
```

The song region is four equal quarters, stem-major:

```
| stem 0 timeline | stem 1 timeline | stem 2 timeline | stem 3 timeline |
```

**Partial final group:** if `frames` is not a multiple of 510, the last group of
each stem is partially filled. **Zero-pad the remainder** — write silence, not
stale bytes. The firmware pads identically and a test asserts it.

**Do not pad a stem's quarter up to a multiple of 4 groups** to make groups
align with sector boundaries. They do not align in general, and that is correct:
each group names its own stem, so nothing has to infer it. Padding would change
the song's size and break every stored geometry field.

---

### 7. Checksums

Two different algorithms. Do not mix them up.

**Content checksums — FNV-1a, 32-bit:**

```ts
function checksum32(bytes: Uint8Array, h = 0x811c9dc5): number {
  for (const b of bytes) { h = ((h ^ b) * 0x01000193) >>> 0; }
  return h >>> 0;
}
```

- `stemChecksums[s]` = FNV-1a over **stem s's own contiguous PCM in playback
  order**, at the full padded `frames` length — L then R, 4 bytes per frame.
  Computed from the samples, **never** from assembled group or sector bytes.
- `songChecksum` = FNV-1a over the **16-byte digest** formed by writing the four
  `stemChecksums` as u32 little-endian, in stem order.

Because the checksums come from the samples and not the layout, moving bytes
around on storage moves no checksum. That is the whole claim the format rests
on — if a checksum changes when only the layout changed, something is wrong.

**Record integrity — CRC-32 (IEEE 802.3),** the ordinary one, used only for the
index record's own `crc32` field. See §8.

---

### 8. The STIX v2 index record — 256 bytes

One record per index region, written into a 512-byte block. **Bytes [256,512)
of that block must be zero.**

| offset | type | field |
|---|---|---|
| 0 | u32 | magic — **`0` while uncommitted**, `0x53544958` (`'STIX'`) once committed |
| 4 | u16 | indexVersion = `2` |
| 6 | u16 | formatMajor = `1` |
| 8 | u16 | **formatMinor = `3`** |
| 10 | u8 | slotIdentity — 0=A, 1=B; must equal the region it is written to |
| 11 | u8 | songSlot — 0=A, 1=B |
| 12 | u16 | flags — bit0 = SONG_PRESENT |
| 16 | u32 | generationLo |
| 20 | u32 | generationHi |
| 24 | u32 | songStartBlock |
| 28 | u32 | songBlockCount |
| 32 | u32 | frames |
| 36 | u32 | sectorCount — **= `groups`** (see below) |
| 40 | u32 | sampleRate = `48000` |
| 44 | u16 | channels = `2` |
| 46 | u16 | **bitDepth = `16`** |
| 48 | u32 | bpmQ8 — BPM × 256 |
| 52 | u32 | downbeatFrame |
| 56 | u32 × 4 | originalFrames, one per stem |
| 72 | u32 × 4 | stemChecksums, one per stem |
| 88 | u32 | songChecksum |
| 92 | 60 bytes | title, UTF-8, NUL-padded |
| 152 | 60 bytes | artist, UTF-8, NUL-padded |
| 252 | u32 | crc32 — the record's own integrity CRC, **last field** |

`sectorCount` keeps its name for compatibility but under this layout it is the
number of **groups per stem**. The firmware validates
`sectorCount == ceil(frames / 510)` and
`songBlockCount == sectorCount * 16`, and refuses the record otherwise.

**`bitDepth` must be 16, and it is checked against the width `formatMinor`
implies.** A record whose declared width disagrees with its own declared version
is malformed and is rejected.

**The CRC rule:** CRC-32 over bytes `[0, 252)` **with bytes `[0,4)` — the magic
— treated as zero**, regardless of what the magic actually holds. So the same
CRC is valid before and after the magic is written. That is what makes the
magic-last commit atomic.

---

### 9. The bulk write command `'U'`

Send `'U'`, then a 17-byte header, then exactly 8192 bytes.

Request header:

| offset | type | field |
|---|---|---|
| 0 | u8 | version = `1` |
| 1 | u32 | seq — 0-based, strictly sequential within one session |
| 5 | u32 | destBlock — absolute first block of this sector |
| 9 | u32 | payloadLen — must be `8192` |
| 13 | u32 | payloadCrc32 — CRC-32 of the 8192 bytes that follow |

Response, 14 bytes:

| offset | type | field |
|---|---|---|
| 0 | u8 | status — `0` = OK |
| 1 | u32 | seq, echoed |
| 5 | u32 | destBlock, echoed |
| 9 | u32 | **verifiedCrc32 — computed from the bytes READ BACK off storage** |
| 13 | u8 | retryable — 1 = resending this exact request may succeed |

`verifiedCrc32` matching your `payloadCrc32` **is** the read-back verification.
You never need a separate 512-byte-at-a-time readback pass for song data.

**Sequencing:** `seq` must be the session's next expected value, or the
immediately preceding already-accepted value (a lost-ACK retry — always safe,
rewriting the same bytes to the same block is idempotent). Anything else fails
closed. `destBlock` must independently agree with what `seq` implies
(`regionStart + seq * 16`); a request with the right sequence number and the
wrong destination is rejected, never guessed at.

**Upload sectors in ascending `seq` from 0.** Sector `s` carries the four groups
whose flat stem-major ordinals are `4s … 4s+3`. This is not cosmetic: the
firmware folds the commit checksums in as each sector's read-back arrives, which
is what makes the commit itself instant. Out-of-order sectors still *work* — the
device falls back to a full re-read and the upload is still correct — they are
just slow.

Status codes worth surfacing distinctly: `5` CRC mismatch, `7` no session open,
`8` session already committed, `9` out of sequence, `10` destination mismatch.

---

### 10. The safe replacement sequence — 22 steps

Never write to the active song or active index region. Uploads alternate:
song A/index B, then song B/index A, then song A/index B, …

1. Re-query `'Q'` immediately before writing. Every immutable field must equal
   the negotiated set, or write nothing.
2–4. Read index A, read index B, run the selector (§11).
5. Destination = **inactive** song slot + **inactive** index slot.
   `generation = active generation + 1`.
6. Capacity check on the inactive slot. If it is too small, raise an
   insufficient-capacity error **before any write**. The active song is never
   overwritten to make a replacement fit.
7. Assert destination ≠ active song slot and destination ≠ active index slot.
8. Write the song into the inactive song region with `'U'`, ascending `seq`,
   retrying a failed sector up to 3 times.
9–12. `verifiedCrc32` already proves each sector landed. Recompute the four stem
   checksums and the song checksum from what you sent.
13. Write the **uncommitted** index record — `magic = 0` — with `'W'`.
14. `'F'` flush.
15–16. Read it back and verify every byte except the intentionally absent magic,
    plus the zero padding out to 512.
17. Write the validity magic. **This is the last write of the sequence.**
18. `'F'` flush.
19–21. Re-read **both** index records, run the selector again, and require the
    new generation to be selected and to match this song field by field.
22. Only now report `committed`.

---

### 11. The active-index selector

Given both index blocks, a record is **valid** only if all of:

- `magic == 'STIX'`
- `crc32` matches the rule in §8
- `indexVersion == 2`, `formatMajor == 1`, `formatMinor == 3`
- `slotIdentity` equals the region it was read from
- if SONG_PRESENT: geometry self-consistent (§8) and inside that song slot's
  real bounds from the `'Q'` reply
- if not SONG_PRESENT: every song/audio field zero

Among valid records, the **higher 64-bit generation** wins. If neither is valid,
the library is `corrupt`.

---

### 12. Outcomes — and the words for them

| condition | outcome | meaning |
|---|---|---|
| step 22 reached | `committed` | new generation verified on the device |
| failure **before** step 17 | `failed` | no magic sent; previous generation still active |
| failure **at or after** step 17 | `unknown` | magic may have landed; reconnect decides |
| both records invalid | `corrupt` | blank or damaged; explicit init required |

**`unknown` is never terminal and must never be presented as data loss.** On
reconnect, read both records, run the same selector, and resolve it to
`committed` or `failed`. A valid previous generation means the replacement
simply did not commit.

Do not use the words "erased", "wiped", "lost", "corrupted" or "reset" for an
interrupted upload. The previous song is intact by construction — say that.

---

### 13. Initialization

Explicit and user-confirmed only, and legal **only** when both index records are
invalid or blank. Write index B as explicit zeros, then a valid song-free
generation-1 record into index A (uncommitted → flush → magic → flush). Create
no false song entry; leave both song regions free for the first upload.

---

### 14. Acceptance tests — please run these and report the numbers

1. **Geometry.** For a song of 14592 frames: `groups == 29`,
   `songBlockCount == 464`, and the last group of each stem holds 312 real
   frames with the remaining 198 zero-filled.
2. **Group tiling.** Every emitted group is exactly 2048 bytes, starts
   `50 4C <stem> 03`, and its `groupIndex` matches its position.
3. **Checksums are layout-independent.** Compute the five checksums from the
   samples; then compute them again by walking the assembled groups back into
   playback order. They must be identical.
4. **Round-trip.** Decode your own emitted groups back to samples and compare
   against the input, sample for sample. Exact equality.
5. **Rounding.** Assert `to16(0x7FFFFF) === 32767`, `to16(-0x800000) === -32768`,
   `to16(127) === 0`, `to16(128) === 1`, `to16(-128) === 0`, `to16(-129) === -1`.
6. **Version refusal.** With a mocked `'Q'` reply carrying `formatMinor = 2`,
   the app must refuse to upload and must not send a single write.
7. **Interruption.** Kill the connection between step 17 and step 18 in a mock
   and confirm the app reports `unknown`, then resolves it correctly on
   reconnect.

---

### 15. What to send back

- The five checksums and the geometry numbers for one real song.
- The first group of each stem as hex (first 32 bytes is enough).
- The complete 256-byte index record as hex, for one committed upload.
- Confirmation that tests 1–7 pass, with the actual asserted values.

## END OF PROMPT

---

## Notes for us, not for Lovable

**Cross-checking the answer.** The firmware repo can verify a companion-produced
image directly: `tools/stemtape-v13-convert.py` produces the reference 16-bit
image from the frozen 24-bit fixture, and
`handoff/v1.3/binaries/song-sectors-four-stem.bin` is that image, 29 sectors,
237,568 bytes. If Lovable's encoder is given the same source stems it must
produce a byte-identical song region.

**What the five checksums cannot prove.** They are computed from samples, so
they are identical for the interleaved and planar layouts of the same song —
that is deliberate, and it means matching checksums do **not** prove the layout
is right. Only a byte-level comparison of the assembled region does. Acceptance
test 4 above is the one that catches a layout error.

**The one weakened claim, recorded honestly.** The v1.3 fixture's checksums were
computed by an independent implementation of the FNV-1a rule, not by the
companion on the other side of the contract — see
`docs/stem-tape-v11-conformance-retirement.md`. Once Lovable's encoder exists
and agrees, that is the third implementation, and the strongest lost claim is
restored at no cost.
