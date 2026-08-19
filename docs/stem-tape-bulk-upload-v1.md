# Stem Tape bulk verified-sector upload — wire contract v1

Status: **Slice C3 — wire contract frozen, host-tested, wired into the
real production firmware dispatcher, AND the post-commit runtime reload
(new song selectable and playable without reflashing/rebooting) is
implemented and host-verified.** Removing the now-superseded `'Y'`
benchmark command and the final CI/handoff proof bundle is Slice C4. This
document is extended, not replaced, by later slices — see the
end-of-document changelog.

## 0. Why this exists

The production song-data write path currently uses the unchanged classic
Tape Looper `'W'` command, one 512-byte physical block per round trip. A
real 248.5 MiB, 4-stem song is 509,024 physical blocks — 509,024
separately-acknowledged writes. Three real physical upload attempts failed
partway through (at block offsets 499, 609, and 633 blocks into the
inactive region) using this path. Safety was never compromised (no
validity magic was ever written; the previous generation stayed active in
every case) — but transport reliability at this granularity is not
adequate for a real song.

This contract adds **one new command, `'U'`**, that transfers, writes, and
verifies one **complete 8192-byte Stem Tape v1.1 sector** (16 physical
blocks) per round trip — 31,814 round trips for the same real song, using
the eMMC driver's real multi-block program/read operations
(`emmc_write_blocks()`/`emmc_read_blocks()` with `count = 16`, real CMD25
bursts) instead of sixteen independent single-block `CMD24` operations.

**Additive only.** `'P'`/`'Q'`/`'R'`/`'W'`/`'F'`/`'X'` are unchanged. `'W'`
remains the *only* way to write a STIX v2 index record — this command is
never used for index blocks, and the existing magic-commit detection
(`xfer_v11_write()`) is untouched.

## 1. Command byte

`'U'` (`0x55`). Checked against the complete real dispatcher
(`xfer_service()` in `firmware/stemtape_player/src/main.c`) before
selection: the live dispatcher recognizes exactly `P`/`R`/`W`/`F`/`X`/`Q`
(plus the T0-slice diagnostic `Y`, removed once this path is production —
see §6). `U` was never used by any prior or current version of this
contract, including the retired v1.0 Gate 2 verbs (`V`/`B`/`S`/`K`/`C`/`A`/
`D`/`I`, all deleted from the dispatcher, and the removed `Z` verb) — `U`
is a fresh choice, not reused from history, to avoid any ambiguity for a
reader of old documentation or old transcripts.

## 2. Wire format

```
host   -> device: 'U' <request header, 17 bytes, LE> <payload, exactly 8192 bytes>
device -> host:   <response, 14 bytes, LE>
```

### 2.1 Request header (17 bytes)

| field | offset | size | meaning |
| --- | ---: | ---: | --- |
| version | 0 | 1 | must equal `1` (`ST_BULK_PROTO_VERSION`) |
| seq | 1 | 4 | transaction/sequence number, 0-based, strictly sequential per open session |
| dest_block | 5 | 4 | absolute physical block this sector's first block must land at |
| payload_len | 9 | 4 | must equal `8192` |
| payload_crc32 | 13 | 4 | CRC-32 (IEEE 802.3, same algorithm as the rest of this contract's own `st_crc32.c`) of the 8192-byte payload that follows |

All multi-byte fields little-endian, matching every other numeric field in
this contract family.

Immediately following the 17-byte header: exactly 8192 bytes of payload —
one complete Stem Tape v1.1 STSC sector (32-byte header + 8160-byte frame
payload, per `docs/stem-tape-transfer-v1.1.md` §8), unchanged format.

### 2.2 Response (14 bytes)

| field | offset | size | meaning |
| --- | ---: | ---: | --- |
| status | 0 | 1 | see §3 |
| seq | 1 | 4 | echoed from the request |
| dest_block | 5 | 4 | echoed from the request |
| verified_crc32 | 9 | 4 | CRC-32 the device actually computed from the bytes it read back off eMMC after writing (`0` if the write/read-back never happened) |
| retryable | 13 | 1 | `1` = resending this exact request is safe and may succeed; `0` = the request itself is structurally wrong, resending it verbatim cannot help |

**`verified_crc32` replaces the separate 512-byte-at-a-time read-back pass
for song data.** Comparing it against the request's own `payload_crc32`
*is* the read-back verification; on success (`status = 0`) the two are
always equal. The device never sends all 8192 read-back bytes back over
USB — only this 4-byte digest.

## 3. Status codes

| code | name | meaning | retryable |
| ---: | --- | --- | :---: |
| 0 | OK | committed to eMMC, read back, and CRC-verified | — |
| 1 | ERR_UNSUPPORTED_VERSION | request header's `version` field is not `1` | no |
| 2 | ERR_BAD_LENGTH | `payload_len` is not `8192` | no |
| 3 | ERR_TIMEOUT_PAYLOAD | payload receive stalled/timed out before all 8192 bytes arrived | yes |
| 4 | ERR_CDC_OVERFLOW | the CDC RX ring dropped bytes during this payload's receive | yes |
| 5 | ERR_CRC_MISMATCH | the received payload's own CRC-32 does not match declared `payload_crc32` | yes |
| 6 | ERR_LAYOUT_NOT_READY | no v1.1 layout at all on this device | no |
| 7 | ERR_NO_SESSION | no v1.1 write session currently open (re-query `'Q'`) | no |
| 8 | ERR_SESSION_CLOSED | the open session already committed (single-use latch already tripped) | no |
| 9 | ERR_OUT_OF_SEQUENCE | `seq` is neither the expected next value nor the one legal retry | no |
| 10 | ERR_DEST_MISMATCH | `seq` is acceptable but `dest_block` disagrees with what it implies | no |
| 11 | ERR_OUT_OF_BOUNDS | this sector would run past the frozen inactive region's own capacity | no |
| 12 | ERR_ACTIVE_REGION | destination resolves to the active song or index region | no |
| 13 | ERR_OUTSIDE_FROZEN_PAIR | destination is a v1.1 address but outside this session's frozen pair | no |
| 14 | ERR_EMMC_WRITE_FAIL | the real multi-block eMMC program operation failed | yes |
| 15 | ERR_EMMC_READBACK_FAIL | the real multi-block eMMC read-back operation failed | yes |
| 16 | ERR_READBACK_CRC_MISMATCH | bytes actually read back off eMMC do not CRC-match what was written | yes |

Values are frozen wire constants — a future extension may only append, never
renumber.

## 4. Sequencing and idempotency

`seq` starts at `0` for the first sector of a session and increments by
exactly `1` per accepted sector. `dest_block` for a given `seq` is always
`region_start + seq * 16`, where `region_start` is this session's frozen
inactive song region's own base block (from the most recent `'Q'` reply's
`songAStart`/`songBStart`, whichever the reply names as inactive).

A request is accepted as a **genuinely new sector** only if `seq` equals
the session's next expected value.

A request is accepted as a **legal idempotent retry** — safe to reprocess
in full, including rewriting the same bytes to the same block — only if
`seq` equals the *immediately preceding, already-accepted* value (i.e. the
host never received the previous ack and is resending it verbatim). A
retry never advances the session's sequence tracker again.

Any other `seq` value — including a retry of anything older than the most
recently accepted sector, or a `seq` further ahead than the expected next
value — is rejected as `ERR_OUT_OF_SEQUENCE`, unconditionally. A `dest_block`
that disagrees with what an otherwise-acceptable `seq` implies is rejected
as `ERR_DEST_MISMATCH`. The device never advances or commits state on the
strength of an acknowledgement alone — only a genuinely verified
write+read-back+CRC round trip ever advances the sequence tracker.

The sequence tracker resets to "expect `seq = 0` at the region's own start
block" every time a v1.1 write session (re)opens — i.e. every real `'Q'`,
matching `xfer_v11_refresh_session()`'s existing cadence and docs §5 step 1
("re-query `Q` immediately before writing").

## 5. Safety boundary (unchanged from the rest of this contract family)

Every block this command writes is validated the same way `xfer_v11_write()`
already validates every `'W'` block: through `st_ab_session_check_write()`
against the currently open session's frozen inactive destination pair — the
active song and active index are rejected outright, and anything outside
the frozen pair is rejected. This command's own sequence tracker (§4) is an
early, cheap, redundant-but-harmless fast-rejection; it is never the
authoritative bounds/active-region gate. Index records are never written
through this command — `'W'` remains the sole path for the STIX v2 index
region, unchanged.

## 6. Q/STCP capability negotiation

The existing, byte-for-byte frozen `'Q' -> STCP` 100-byte reply (`docs/
stem-tape-transfer-v1.1.md` §2/§12.5, verified against `handoff/v1.1/
binaries/stcp-capability-response.bin`) is **not** modified by this
contract — that fixture stays byte-exact; `st11_stcp_build()` itself is
never touched. Capability negotiation for this command is a separate,
explicitly-tagged **12-byte extension block**, appended immediately after
the existing 100-byte reply, in the same continuous `'Q'` response (one
transmission, 112 bytes total):

| field | offset (within the 12-byte block) | size | meaning |
| --- | ---: | ---: | --- |
| tag | 0 | 4 | literal ASCII `"STBC"` |
| flags | 4 | 4 | bit 0 = `ST_BULK_CAP_FLAG_SUPPORTED` (always set — there is no partial/conditional support once this command exists in a build at all) |
| max_sector_bytes | 8 | 4 | `8192` — the exact payload size this command accepts per call |

The companion must check for this tag explicitly rather than inferring
support from `protoMinor`/`formatMinor` or any other version field. This
block is additive at the tail of the `'Q'` response; it does not change
the length or content of anything already there.

Sent (or not) exactly when the original 100-byte reply is sent (or not):
if `g_v11_layout_ready` is false, neither part is transmitted.

## Changelog

- **Slice C1**: wire format, status codes, sequencing rules frozen. Host-
  tested (`st_bulk_xfer.c`/`test_bulk_xfer.c`) against a real frozen
  sector from `handoff/v1.1/binaries/song-sectors-four-stem.bin` and the
  real region geometry from `handoff/v1.1/decoded/
  stcp-capability-response.json`. Not yet linked into the real firmware
  target.
- **Slice C2** (this revision): `xfer_bulk_write_sector()` wired into the
  real `xfer_service()` dispatcher in `main.c` — receives with correct
  backpressure and CDC-overflow detection, validates CRC before touching
  eMMC, validates all 16 physical blocks through the real
  `st_ab_session_check_write()` gate (the exact same one `'W'` uses),
  writes via one real `emmc_write_blocks(..., 16)` burst, reads back via
  one real `emmc_read_blocks(..., 16)` burst, and only acknowledges
  success once the read-back CRC matches. §6 (Q/STCP capability
  extension) specified and wired into `xfer_v11_send_caps()`.
  `st_bulk_xfer.c` now linked into the real Zephyr target
  (`CMakeLists.txt`), and `ALLOWED_WRITE_FUNCS` in the strict persistence
  safety gate extended to structurally prove
  `xfer_bulk_write_sector()`'s own body genuinely bounds every write
  through the real session gate before it can ever reach
  `emmc_write_blocks()`.
- **Slice C3** (this revision): post-commit runtime reload, so a newly
  uploaded song becomes selectable and playable without reflashing or
  rebooting. `xfer_v11_write()` now recognizes its own magic-committing
  write (REPLACE or INIT alike) the instant it genuinely lands on eMMC
  and sets a one-shot `g_v11_commit_pending` flag — no new wire command;
  the very next real `'F'` (durability flush), which docs §5 step 18
  always places immediately after the magic write, consumes that flag
  (after sending the flush ack, never delaying it) and calls
  `stem_song_post_commit_reload()`. That function re-reads both index
  blocks fresh, re-runs the real `st_stix_read_library()` selector (never
  trusting the write just performed), and — if the newly selected record
  names a song — reads its real sector 0 and validates it through a
  local, throwaway `st_stream_t` before ever publishing a reload request.
  The actual reconstruction of the shared, audio-thread-owned
  `g_stem_stream`/`g_stem_mbox`/`g_stem_beat_timing` is performed by
  `looper_audio_block()`'s own PASS C, once per audio block, the same
  thread that already exclusively owns those structures after boot —
  this handoff is safe specifically because `g_playing` is forced to 0
  for the entire duration of any transfer session (set the instant the
  `SP1XFER!` magic is detected), so stored-song playback is provably not
  contending with upload writes or with this reload at any point (item
  12's requirement). Any failure along the reload path leaves the
  previously selected song, if any, completely undisturbed — never a
  partial or torn selection — and increments a diagnostic-only
  `g_stem_reload_fail_count` (`controls_diag()`'s existing `STEMIO`
  line), never a user-facing signal. Verified via a new long-run host
  test (`test_bulk_xfer_walk.c`) that drives the real
  `st_bulk_seq_check()`/`st_ab_session_check_write()`/
  `st_bulk_seq_advance()` functions through a full 31,814-sector
  (509,024-block) walk against a mock eMMC that remembers only the
  single most-recently-written sector (O(1) memory, no 248.5 MiB
  fixture) — including the exact offsets (499/609/633 blocks into the
  inactive region) the phase directive's own physical failure report
  named.
