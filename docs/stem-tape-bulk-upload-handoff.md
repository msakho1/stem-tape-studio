# Stem Tape bulk verified-sector upload — Lovable handoff

This is the exact, implement-without-guessing contract for the companion
side of the `'U'` bulk verified-sector upload command. It is generated
directly from the real, CI-verified firmware source
(`firmware/stemtape_player/src/st_bulk_xfer.h`/`.c`,
`firmware/stemtape_player/src/main.c`'s `xfer_bulk_write_sector()`) — every
byte sequence below was produced by compiling and running that real code
against a real frozen fixture, never hand-typed or guessed. The full
prose spec this document summarizes lives in
`docs/stem-tape-bulk-upload-v1.md`; read that for the complete safety
argument, concurrency design, and idempotency rules. This document exists
so the companion team can implement the wire client without reading
firmware source at all.

**Status: firmware side DONE and CI-verified.** Companion implementation
is the next step. Physical upload success has NOT yet been demonstrated —
see the Definition of Done note at the end of this document.

## 1. Command byte

`'U'` — `0x55`.

## 2. Request: host → device

```
'U' (1 byte) + <17-byte header, all fields little-endian> + <8192-byte payload>
```

### 2.1 Request header (17 bytes)

| field | offset | size | type | meaning |
| --- | ---: | ---: | --- | --- |
| `version` | 0 | 1 | `u8` | must equal `1` |
| `seq` | 1 | 4 | `u32` LE | transaction/sequence number, 0-based, strictly sequential per open session |
| `dest_block` | 5 | 4 | `u32` LE | absolute physical block this sector's first block must land at |
| `payload_len` | 9 | 4 | `u32` LE | must equal `8192` |
| `payload_crc32` | 13 | 4 | `u32` LE | CRC-32 (IEEE 802.3 / CRC-32/ISO-HDLC — the same polynomial used by zlib's `crc32()`, Python's `zlib.crc32()`, and JavaScript's common `crc-32` npm package) of the 8192-byte payload that follows |

Immediately following the header: exactly **8192 bytes** of payload — one
complete Stem Tape v1.1 STSC sector (32-byte sector header + 8160-byte
frame payload, unchanged format, per `docs/stem-tape-transfer-v1.1.md`
§8).

### 2.2 CRC-32 algorithm, precisely

IEEE 802.3 CRC-32 (polynomial `0xEDB88320` reflected form), computed over
the raw 8192 payload bytes with no pre/post transformation beyond the
standard init `0xFFFFFFFF` / final XOR `0xFFFFFFFF` — identical to
`zlib.crc32()`, Python's `binascii.crc32()`/`zlib.crc32()`, and the
`crc-32` npm package's default. If your CRC-32 implementation matches any
of those, it matches this contract.

## 3. Response: device → host (14 bytes, always exactly 14 bytes)

| field | offset | size | type | meaning |
| --- | ---: | ---: | --- | --- |
| `status` | 0 | 1 | `u8` | see §4 |
| `seq` | 1 | 4 | `u32` LE | echoed from the request |
| `dest_block` | 5 | 4 | `u32` LE | echoed from the request |
| `verified_crc32` | 9 | 4 | `u32` LE | CRC-32 the device computed from the bytes it actually read back off eMMC after writing (`0` if the write/read-back never happened) |
| `retryable` | 13 | 1 | `u8` | `1` = resending this exact request is safe and may succeed; `0` = the request is structurally wrong, resending it verbatim cannot help |

**`verified_crc32` replaces the separate 512-byte-at-a-time read-back pass
for song data entirely.** The device never sends the 8192 read-back bytes
back over USB — only this 4-byte digest. On success (`status = 0`),
`verified_crc32` always equals the request's own `payload_crc32`.

## 4. Status codes (frozen, append-only)

| code | name | meaning | retryable |
| ---: | --- | --- | :---: |
| 0 | `ST_BULK_OK` | committed to eMMC, read back, and CRC-verified | — |
| 1 | `ERR_UNSUPPORTED_VERSION` | `version` field is not `1` | no |
| 2 | `ERR_BAD_LENGTH` | `payload_len` is not `8192` | no |
| 3 | `ERR_TIMEOUT_PAYLOAD` | payload receive stalled/timed out before all 8192 bytes arrived | **yes** |
| 4 | `ERR_CDC_OVERFLOW` | the CDC RX ring dropped bytes during this payload's receive | **yes** |
| 5 | `ERR_CRC_MISMATCH` | the received payload's own CRC-32 does not match declared `payload_crc32` | **yes** |
| 6 | `ERR_LAYOUT_NOT_READY` | no v1.1 layout at all on this device | no |
| 7 | `ERR_NO_SESSION` | no v1.1 write session currently open — re-query `'Q'` | no |
| 8 | `ERR_SESSION_CLOSED` | the open session already committed (single-use latch already tripped) | no |
| 9 | `ERR_OUT_OF_SEQUENCE` | `seq` is neither the expected next value nor the one legal retry | no |
| 10 | `ERR_DEST_MISMATCH` | `seq` is acceptable but `dest_block` disagrees with what it implies | no |
| 11 | `ERR_OUT_OF_BOUNDS` | this sector would run past the frozen inactive region's own capacity | no |
| 12 | `ERR_ACTIVE_REGION` | destination resolves to the active song or index region | no |
| 13 | `ERR_OUTSIDE_FROZEN_PAIR` | destination is a v1.1 address but outside this session's frozen pair | no |
| 14 | `ERR_EMMC_WRITE_FAIL` | the real multi-block eMMC program operation failed | **yes** |
| 15 | `ERR_EMMC_READBACK_FAIL` | the real multi-block eMMC read-back operation failed | **yes** |
| 16 | `ERR_READBACK_CRC_MISMATCH` | bytes actually read back off eMMC do not CRC-match what was written | **yes** |

A future firmware revision may only *append* new codes here, never
renumber or repurpose an existing one.

## 5. Sequencing and retry rules

- `seq` starts at `0` for the first sector of a session and increments by
  exactly `1` per accepted sector.
- `dest_block` for a given `seq` is always `region_start + seq * 16`,
  where `region_start` is the CURRENT inactive song region's own base
  block (from the most recent `'Q'` reply's `songAStart`/`songBStart`,
  whichever the reply names as inactive). **Re-query `'Q'` immediately
  before starting an upload** — this also (re)opens the session and resets
  the expected `seq` to `0`.
- A request is accepted as a **genuinely new sector** only if `seq` equals
  the session's next expected value.
- A request is accepted as a **legal idempotent retry** — safe to resend
  in full, including the identical 8192-byte payload — only if `seq`
  equals the *immediately preceding, already-accepted* value (i.e. you
  never received the previous response and are resending it verbatim).
  **A retry never advances the sequence a second time**, and its response
  is byte-identical to the original successful response (see the
  transcript in §7.2).
- Any other `seq` — a retry of anything older than the most recently
  accepted sector, or a `seq` further ahead than expected — is rejected
  as `ERR_OUT_OF_SEQUENCE`, unconditionally.
- The device **never** advances or commits state on the strength of an
  acknowledgement alone — only a genuinely verified write+read-back+CRC
  round trip ever advances its sequence tracker. This means: if you are
  unsure whether your last request was received and processed, **it is
  always safe to resend it** — either it lands on the current expected
  `seq` (processed as new) or the immediately preceding one (processed as
  a no-op-equivalent retry). It is never safe to skip ahead speculatively.

## 6. Timeout and retry policy (host-side recommendation)

The device times out its own payload receive internally (`ERR_TIMEOUT_
PAYLOAD`) — this is a device-side safety bound, not something the host
configures. Host-side guidance:

- If no response arrives within a reasonable window (recommend **5
  seconds** per 8192-byte sector, generous relative to real USB CDC
  throughput), **resend the identical request** (same `seq`, same
  `dest_block`, same payload bytes, same CRC). Per §5, this is always
  either a fresh accept or a safe no-op retry.
- On a response with `retryable = 1`, resending the identical request is
  the correct recovery — no re-query of `'Q'`, no change to the payload.
- On a response with `retryable = 0`, resending verbatim cannot help —
  this indicates either a protocol/logic bug in the host implementation
  (wrong `seq`/`dest_block` arithmetic, stale session) or a state change
  on the device (e.g. `'X'` was sent, closing the session). Re-query
  `'Q'` and restart the upload from `seq = 0` against the freshly reported
  inactive region.
- There is no host-configurable retry *count* limit in the firmware
  contract; a well-behaved host should still cap its own retry attempts
  and surface an error to the user rather than looping forever against a
  device that is genuinely failing (e.g. a real eMMC hardware fault
  producing repeated `ERR_EMMC_WRITE_FAIL`).

## 7. Real transcripts

Every byte sequence below was generated by compiling and running the
real, unmodified `st_crc32.c`/`st_bulk_xfer.c` firmware source against
the real frozen fixture `handoff/v1.1/binaries/song-sectors-four-stem.bin`
(sector 0 — the first real 8192-byte STSC sector of the real prepared
four-stem song fixture this whole contract family is tested against).
Nothing here is hand-typed or fabricated.

Sector 0's real CRC-32: **`0xdfe2813a`**.

### 7.1 Complete successful transcript

Session state: a `'Q'` reply has just reported the inactive song region
starting at block `16` (this document's own worked example — the real
device's actual region start depends on its own real A/B layout, reported
fresh by every `'Q'`). This is the very first sector of the upload,
`seq = 0`.

**host → device:**
```
'U' (0x55) + request header (17 bytes):
01000000 00100000 00002000 003a81e2 df
```
Byte-by-byte: `version=01`, `seq=00000000`, `dest_block=00100000` (LE for
`16`), `payload_len=00002000` (LE for `8192`), `payload_crc32=3a81e2df`
(LE for `0xdfe2813a`).

```
... followed by the real 8192-byte payload (sector 0's own bytes, unmodified):
payload[0:32]   = 43535453 00000000 00000000 54010000 00600000 e02e0000 00000000 00000000
payload[8160:8192] = 10d2e622 04e62204 0090d800 a6272d17 d3903d0b 008cd300 8cd3f3cb 04f3cb04
```
(`43 53 53 54` at offset 0 is the STSC sector magic, ASCII "STSC", the
real Stem Tape v1.1 sector header's own first field — confirming this is
a real, well-formed sector, not arbitrary bytes.)

**device → host** (after a real write+read-back+CRC round trip succeeds):
```
response (14 bytes):
00000000 00100000 003a81e2 df00
```
Byte-by-byte: `status=00` (`ST_BULK_OK`), `seq=00000000`,
`dest_block=00100000` (echoed, `16`), `verified_crc32=3a81e2df` (LE for
`0xdfe2813a` — matches the request's own declared CRC, confirming the
read-back bytes are identical to what was sent), `retryable=00`
(meaningless on success, always `0`).

The device's internal sequence tracker now expects `seq = 1` next.

### 7.2 Duplicate-ack-lost retry transcript

Scenario: the response above was sent by the device, but never reached
the host (USB/CDC packet loss, host crash-and-restart mid-wait, etc.).
The host, still holding sector 0's own bytes and not knowing whether the
write actually happened, resends the **identical** request.

**host → device** (identical bytes to §7.1's own request):
```
'U' (0x55) + the SAME 17-byte header + the SAME 8192-byte payload
```

Device-side: `st_bulk_seq_check()` classifies this as `ST_BULK_SEQ_RETRY`
(seq 0 is the immediately-preceding already-accepted value). The device
reprocesses the full write+read-back+CRC pipeline — rewriting the
identical bytes to the identical block is safe by construction — and does
**not** advance its sequence tracker a second time.

**device → host:**
```
response (14 bytes):
00000000 00100000 003a81e2 df00
```
**Byte-for-byte identical to the original response in §7.1.** The
sequence tracker still expects `seq = 1` next (unchanged by the retry).
This is the guarantee that makes blind resend-on-timeout always safe.

### 7.3 Failure transcript (CRC mismatch — payload corrupted in transit)

Scenario: some later sector (shown here as `seq = 3`, an arbitrary
mid-upload example) is corrupted in transit — a single bit flips
somewhere on the wire between host and device.

- Header declares `payload_crc32 = 0xdfe2813a` (the real, uncorrupted
  sector's own CRC, computed by the host BEFORE transmission).
- The device receives 8192 bytes and recomputes their CRC-32 itself:
  `0xd1e212e9` (different — byte 100 was flipped in this example).
- The two values disagree, so the device rejects the sector **without
  ever touching eMMC**.

**device → host:**
```
response (14 bytes):
05030000 00400000 00000000 0001
```
Byte-by-byte: `status=05` (`ERR_CRC_MISMATCH`), `seq=00000003` (echoed),
`dest_block=00004000` (LE for `64` — this example's `seq=3` destination,
`16 + 3*16`), `verified_crc32=00000000` (zero — the write/read-back never
happened), `retryable=01` (**yes** — resending the same sector, hopefully
transmitted cleanly this time, is the correct recovery).

The host should resend `seq = 3` with the same, uncorrupted payload bytes
(re-reading them from its own source rather than trusting whatever it
just sent, in case the corruption originated on the host side).

## 8. Q/STCP capability negotiation

The existing, byte-for-byte frozen `'Q' → STCP` 100-byte reply (`docs/
stem-tape-transfer-v1.1.md` §2/§12.5) is **unmodified** by this contract.
Capability negotiation for `'U'` is a separate, explicitly-tagged
**12-byte extension block**, appended immediately after the existing
100-byte reply, in the same continuous `'Q'` response (one transmission,
112 bytes total when this capability is present).

Real bytes, generated from the real firmware's `st_bulk_build_caps()`:
```
53544243 01000000 00200000
```
| field | offset | size | value | meaning |
| --- | ---: | ---: | --- | --- |
| `tag` | 0 | 4 | `53544243` = ASCII `"STBC"` | identifies this extension block |
| `flags` | 4 | 4 | `01000000` (LE `1`) | bit 0 = `ST_BULK_CAP_FLAG_SUPPORTED`, always set once this command exists in a build at all |
| `max_sector_bytes` | 8 | 4 | `00200000` (LE `8192`) | exact payload size this command accepts per call |

**Check for this tag explicitly** rather than inferring support from
`protoMinor`/`formatMinor` or any other version field. This block is
additive at the tail of the `'Q'` response; it does not change the length
or content of anything already there. It is sent (or not) exactly when
the original 100-byte reply is sent (or not): if the device reports no
v1.1 layout at all, neither part is transmitted (docs section 2's
"silence = stock firmware = read-only" rule).

## 9. Final commit procedure (unchanged from the existing v1.1 contract)

The bulk command **only ever writes song-data sectors** — it never writes
a STIX v2 index record, and never touches the commit/magic mechanism.
Once every song sector has been uploaded and verified via repeated `'U'`
calls (§5–§7), the **existing** v1.1 commit sequence
(`docs/stem-tape-transfer-v1.1.md` §5) applies completely unchanged:

1. Write the inactive STIX index record via the existing `'W'` command,
   **without** the validity magic set (an "uncommitted draft").
2. `'F'` (flush).
3. Read the draft back via `'R'` and validate it.
4. Write the SAME record again via `'W'`, this time **with** the validity
   magic set — this is the sole commit point; the device's own
   `st_ab_session_check_write()` gate refuses this write unless the song
   region was independently verified server-side first
   (`st_ab_session_verify_song_before_commit()`, real I/O, real
   checksums — the firmware never takes the companion's word for it).
5. `'F'` (flush) — **this specific flush**, immediately following a
   real magic-committing write, is also the trigger for the firmware's
   own post-commit runtime reload (Slice C3): the device re-selects the
   new generation and reloads its stored-song streaming state internally,
   with no new wire command needed and no reboot required. By the time
   this flush's response returns, the newly uploaded song is already
   selectable and playable.
6. `'R'` the newly-committed record back one more time and confirm the
   selected generation is the new one, exactly as before.

An interruption at any point before step 4's magic write leaves the
previous generation fully intact and selectable. An interruption after
step 4 but before step 5's flush is durability-barrier-dependent, exactly
as already documented for the single-block `'W'` path — this bulk command
introduces no new interruption window here, since it never participates
in the commit mechanism at all.

## 10. Firmware identity (this slice)

- **Firmware commit:** `b2df682513cc4e2c91f0fbedcae326256682a84b` (branch
  `claude/stemtape-m0-safety-audit-1vg9pq`) — CI-verified green: the real
  `stemtape-player` job (workflow run
  [32292675001](https://github.com/msakho1/stem-tape-studio/actions/runs/32292675001),
  job [96196927124](https://github.com/msakho1/stem-tape-studio/actions/runs/32292675001/job/96196927124))
  completed successfully, including the runtime symbol-presence gate (all
  required v1.1/bulk-upload symbols present, all retired v1.0- and
  T0-benchmark-path symbols confirmed absent) and the strict persistence
  safety gate (`GATE PASSED — no persistent-write capability outside the
  2 proven, session-bounded eMMC adapter function(s)
  (xfer_bulk_write_sector() and xfer_v11_write())`). This commit contains
  the complete upload-reliability phase deliverable: wire contract,
  production dispatcher wiring, post-commit runtime reload, and the `'Y'`
  benchmark removal + safety-gate report corrections (Slices C1–C4).
- **BIN artifact** (from that same real, green CI run — every value
  below is copied directly from the build job's own `sha256sum` output,
  not computed or guessed locally):

  | file | SHA-256 | size |
  | --- | --- | --- |
  | `stemtape_player.bin` | `d9f23369236b7f72b7a904307bd0f4bfbe060ecccff4df878f8a443108bd3f52` | 98,940 bytes |
  | `zephyr.bin` | `d9f23369236b7f72b7a904307bd0f4bfbe060ecccff4df878f8a443108bd3f52` | 98,940 bytes (identical to `stemtape_player.bin`) |
  | `zephyr.elf` | `2175ff816beadc23e8e59b39299e3a9911f56f0bc42979c9655ac830c0e5cd42` | — |
  | `zephyr.hex` | `51751b7ec85cb3e2410a3dcd91ae9748333893ca3c1dbf1a6460dbfa1765f359` | — |

  Downloadable from the `stemtape-player-audit` GitHub Actions artifact:
  <https://github.com/msakho1/stem-tape-studio/actions/runs/32292675001/artifacts/9380234871>
  (requires repo access; artifacts expire ~90 days after the run). Verify
  the downloaded `stemtape_player.bin`'s own SHA-256 against the table
  above before flashing.

## Definition of done — what is proven vs. not

**Proven (CI, host tests, real fixtures):** the wire format round-trips
correctly through the real parser; CRC-32 detects real corruption;
truncated/out-of-order/duplicate requests are classified correctly;
every A/B region boundary is rejected correctly; a real 31,814-sector
(509,024-block) walk completes via the real sequencing/session-gate
functions against a mock eMMC; write/read-back/CRC failure responses are
correct; the post-commit reload logic is host-verified; the real Zephyr
build links this command into the production dispatcher; all existing
safety gates and the golden Tape Looper baseline remain green.

**NOT yet proven:** a real physical upload of the real 248.5 MiB,
four-stem song over a real SP-1's real USB/CDC/eMMC stack. That is the
one claim this document deliberately does not make — it is the
companion's own physical test, once implemented against this contract,
that will produce it.
