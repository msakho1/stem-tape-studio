# Stem Tape Companion Transfer Protocol v1

Protocol version: **1.0.0** (`ST_XFER_PROTOCOL_VERSION`)
Canonical header: [`firmware/stemtape_player/src/st_transfer_protocol.h`](../firmware/stemtape_player/src/st_transfer_protocol.h)
Storage layout: [`firmware/stemtape_player/src/st_storage_layout.h`](../firmware/stemtape_player/src/st_storage_layout.h)
Fixtures for companion tooling: [`docs/stem-tape-transfer-v1-fixtures.json`](./stem-tape-transfer-v1-fixtures.json)

This is a **companion maintenance connection only**. USB is never required for
performance — every control in `docs/FIRMWARE_CONTRACT_V1.md` and the
firmware's standalone gesture grammar works with no host attached. This
protocol exists solely to move songs on and off the device and to run the
one explicit, destructive storage-initialization command.

## 1. Relationship to the classic SP-1 Tape Looper transfer protocol

The Stem Tape standalone player reuses the exact proven Web Serial
block-transfer handshake from the SP-1 Tape Looper firmware
(`firmware/src/main.c`'s `xfer_service()`): a companion tool sends the
8-byte magic `SP1XFER!` over the USB CDC console; on completion of the
match the device pauses audio and enters transfer mode. The classic verbs
(`P` ping/layout, `R` read one block, `W` write one block, `F` flush, `X`
commit-and-exit) are preserved **unchanged, byte-for-byte**, so an existing
companion tool that only knows the classic looper protocol still works for
raw block access.

Everything below this line is **new**, scoped only to what the Stem Tape
song layout requires: a capability/version handshake so a companion tool
never assumes looper semantics against Stem Tape firmware (or vice versa),
and a transactional slot-upload sub-protocol that the classic `R`/`W`
verbs do not provide (the looper's own metadata commit — `xfer_commit()` —
is not transactional against partial track writes the way this format
requires).

## 2. Capability / version handshake

New command `V` (version query), sent once transfer mode is entered:

```
host  -> device: 'V'
device -> host:  16 bytes:
  bytes 0..3   "STV1"                    protocol family + major version
  byte  4      ST_XFER_PROTOCOL_MINOR    (uint8)
  byte  5      ST_STORAGE_LAYOUT_VERSION (uint8)
  bytes 6..7   reserved, 0
  bytes 8..11  capability flags (uint32 LE, ST_XFER_CAP_*)
  bytes 12..15 ST_SECTOR_BYTES (uint32 LE)
```

`ST_XFER_CAP_TRANSACTIONAL_SLOTS` (bit 0) and `ST_XFER_CAP_CRC32` (bit 1)
are both set by every Stem Tape firmware build; a companion tool that does
not see this response (timeout, or a reply not starting `"STV1"`) MUST
assume classic-looper-only semantics and never attempt the slot commands
below.

## 3. Library / slot metadata

See `st_storage_layout.h` for the authoritative struct. Summary:

- The library lives in a **capacity-detected** number of slots
  (`st_storage_compute_slot_capacity()`), never a UI-hardcoded count — the
  firmware reports the real number in the `V`/`P` responses so the
  companion tool never assumes a fixed library size.
- Each slot's committed metadata (`st_slot_meta_t`) records: song id/title
  hash, stem presence bitmask, per-stem sample count, per-stem gain,
  mute/solo/link bitmask, active-stem index, FX bank/algorithm/macro state
  (STEM and GLOBAL, latch bits), selected scrub-speed index, a payload
  CRC32, and start/length in sectors.
- Persisted performance state (current song, per-stem levels, mute/solo,
  active stem, FX selection/latches, scrub speed) lives in the SAME
  slot record it belongs to, committed the same way as everything else —
  there is no separate "settings" write path that could desync from the
  song it describes.

## 4. Audio packing and timing

- **24-bit PCM, 48 kHz, four stereo stems** (Vocal, Drums, Bass,
  Instrument — `ST_STEM_COUNT = 4`, `ST_CHANNELS_PER_STEM = 2`), matching
  the documented stock representation
  (`timknapen/SP-1-dev` wiki, *Audio format* / *Data Structure*). Samples
  are packed 24-bit little-endian, interleaved stem-major then
  channel-major within a frame (`ST_FRAME_BYTES = 24` bytes: stem0 L, stem0
  R, stem1 L, stem1 R, stem2 L, stem2 R, stem3 L, stem3 R). Uploads are
  **never** downgraded to mono or 16-bit by the firmware.
- **8192-byte sectors** (`ST_SECTOR_BYTES`), the documented stock sector
  size — not the classic looper's own 512-byte mono block, which is a
  different, incompatible on-flash format used only by the Tape Looper
  application. The Stem Tape library region and the classic looper's own
  track region (if the same physical device is ever dual-purposed) are
  therefore disjoint address ranges; this protocol never touches looper
  block addresses at all.
- A song's audio payload is `ceil(frame_count * ST_FRAME_BYTES /
  ST_SECTOR_BYTES)` sectors, written sequentially from the slot's `start_sector`.

## 5. CRC / checksum

`crc32` (IEEE 802.3 polynomial, `0xEDB88320` reflected, matching the
classic looper's own use of the same polynomial for its index repair
checks) over:

- every individual staged sector, checked immediately after each `S` write
  (catches a corrupted USB transfer chunk before it is ever committed);
- the complete payload, checked once by `K` (verify) before any commit is
  possible.

## 6. Transactional upload: begin / resume / verify / finalize / abort / replace / delete

New commands, all operating on a single **staging region** (see
`st_storage_layout.h`, `ST_STAGING_SECTOR_COUNT`) that is disjoint from
every committed slot's sectors:

| Cmd | Name | Request payload | Response |
|---|---|---|---|
| `B` | Begin | slot index (u16), frame_count (u64), payload_crc32 (u32), stem presence bitmask (u8) | `b` + staged byte offset to resume from (0 for a fresh begin; nonzero replays a `B` sent again for an interrupted upload — see "Resume" below) |
| `S` | Stage sector | sector index within the transfer (u32), `ST_SECTOR_BYTES` of data, sector crc32 (u32) | `s` (crc verified and written) or `e` (crc mismatch or out of range — the host must resend that sector, the transaction stays open) |
| `K` | Verify | — | `k` (full-payload CRC over the staged sectors matches the `B` payload_crc32) or `e` (mismatch — transaction stays open, nothing is committed) |
| `C` | Commit / finalize | — | `c` (flushed to eMMC, slot metadata written **last**, song now visible) or `e` (refused — verify was not run, or a mismatch was pending) |
| `A` | Abort | — | `a` (staging discarded, target slot's previously committed song, if any, is untouched) |
| `X` (existing) | (unchanged from the classic protocol) also exits transfer mode | — | — |
| `D` | Delete | slot index (u16), destructive-confirmation token (see below) | `d` (slot metadata cleared; sectors are left as-is until reused, never zero-filled synchronously) or `e` |
| `I` | Initialize / convert storage | destructive-confirmation token (see below) | `i` (library header (re)written; **every existing committed slot is discarded**) or `e` |

**Resume.** Because `B` reports the already-staged byte offset for the
requested slot when the SAME `(slot, frame_count, payload_crc32)` tuple is
sent again, a companion tool that lost its connection mid-upload simply
re-sends `B` and continues `S` from the reported offset — it never needs
to restart a large upload from zero. A `B` with a *different* tuple for a
slot that already has an open transaction discards the old staging data
first (one upload attempt at a time per slot).

**Transactional guarantee (the sequence the firmware enforces):**

1. Enter transfer mode (`SP1XFER!` handshake) — audio is paused safely,
   exactly as the classic protocol already does.
2. `B` — write only ever lands in the staging region, never in a
   committed slot's sectors.
3. Every `S` is individually CRC-checked; `K` CRC-checks the whole payload.
4. `C` flushes the eMMC write cache (mirrors the classic protocol's `F`)
   **before** touching slot metadata.
5. Slot metadata (the thing that makes a song visible to playback) is
   written **last**, and only after the flush in step 4 succeeds.
6. The song becomes visible to the playback engine only after `C` returns
   `c`.
7. **On disconnect, timeout (the same 15 s idle timeout the classic
   protocol already uses), `A`, or a power loss at any point before step
   5 completes, the previously committed song for that slot (if any) is
   retained untouched, and the staging data is discarded/ignored** — the
   firmware never auto-resumes a stale staging region as if it were a
   valid song, and never partially exposes it.

**Destructive-confirmation token.** `D` and `I` both require the exact
8-byte token `st_storage_layout.h` defines
(`ST_DESTRUCTIVE_CONFIRM_TOKEN`, distinct from the transfer-mode magic) to
be appended to the request. A request with a missing or wrong token is
rejected (`e`) and performs no action — this is the only way either
command can ever run; there is no bare/short form. **The firmware never
erases or formats storage automatically at boot** under any circumstance,
including a corrupt or unreadable library header — an unreadable header
is treated as "zero songs, read-only until `I` is explicitly sent," never
as an implicit reformat.

## 7. Local LED feedback during transfer

While `g_xfer_mode`-equivalent transfer state is active, all four Track
LEDs blink together locally (matching the classic looper's own transfer
indication), driven by the same firmware-time semantic LED renderer used
for every other pattern (`st_led_pattern.h`, `ST_LED_PATTERN_TRANSFER`) —
not a special-cased blink loop. Exiting transfer mode (`X`, timeout, or
disconnect) restores the **exact** prior local LED state through the same
priority-table mechanism every other temporary pattern uses.

## 8. Machine-readable fixtures

`docs/stem-tape-transfer-v1-fixtures.json` mirrors every constant in this
document (magic bytes, command bytes, struct field offsets/widths, CRC
polynomial, capability flags) for the companion tool, generated by hand
from the same header `st_transfer_protocol.h` the firmware compiles —
keep both in sync if either changes.

## 9. Evidence

- `firmware/src/main.c` (`xfer_service()`, `xfer_commit()`, `xfer_resync()`)
  — the proven classic block-transfer implementation this protocol
  extends, including the magic handshake, 15 s idle timeout, and
  torn-write-safe two-block metadata commit pattern this document's
  "slot metadata written last" rule is directly modeled on.
- `timknapen/SP-1-dev` wiki — *Audio format*, *Data Structure*, *Album
  metadata format* — basis for the 24-bit/48 kHz/four-stereo-stem/
  8192-byte-sector representation in section 4.
- `docs/FIRMWARE_CONTRACT_V1.md` — persisted performance-state fields
  (mixer, mute/solo, active stem, FX, scrub speed) mirrored in
  `st_slot_meta_t`.

No physical device has been used to transfer a song under this protocol.
The transactional state machine (`st_transfer.c`) is verified by host
tests against a mocked sector-storage backend; the real eMMC read/write
binding is deferred — see the firmware README's "Deferred beyond this
release" section.
