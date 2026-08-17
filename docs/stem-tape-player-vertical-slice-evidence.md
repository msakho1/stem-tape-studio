# Stem Tape Player — vertical slice evidence

Scope: upload → validate → persist → power-cycle → USB-disconnected PLAY →
fader control → real LEDs, on the real derived runtime
(`firmware/stemtape_player/src/main.c`, a literal derivative of the proven
Tape Looper — see the file's own provenance banner). Heads Mode, FX,
MIDI/external clock, recording/overdubbing, browser changes, automatic stem
separation and complex gesture chords are explicitly out of scope for this
milestone.

This document is the required evidence package: real runtime call-path
tracing, ELF/link proof, the deterministic test fixture, the automated test
matrix and its honest coverage boundary, and the physical-hardware
validation procedure. Every citation below is `file:line` in this
repository at commit `28a832775d5806d49d5054c314339064d718e600`, and every
build/link/test claim is reproduced from a real CI job log (job IDs cited
inline), not asserted from source reading alone.

## 1. Runtime call-path tracing

| Checkpoint | Function (file:line) | Reached from | Evidence |
|---|---|---|---|
| Startup | `main()` (`main.c:4717`) | Zephyr `k_sys_init`/app entry | linked, checked by the symbol-presence gate (§2) |
| ↳ storage bring-up + streamer start | `streamer_start()` (`main.c:2174`), called from `main()` at `main.c:4791` | `main()` | `k_thread_create(&streamer_tcb, ..., streamer_thread, ...)` at `main.c:2178` |
| ↳ audio bring-up | `audio_init()` (`main.c:3672`), called from `main()` at `main.c:4894` | `main()` | creates `audio_thread` at `main.c:3679`; also calls `streamer_start()` again at `main.c:3694` (idempotent, guarded by `g_streamer_started`) |
| Cold-boot song/index discovery | `streamer_thread()` (`main.c:2770`), block-0 read at `main.c:2829` | thread entry, before its `while(1)` | `emmc_read_blocks(META_BLOCK, metabuf, META_BLOCKS)`; magic/slot validity check; format-fresh fallback via `meta_write_blocks()` if invalid |
| Main loop (transfer service) | `streamer_thread()`'s `while(1)` (`main.c:2858` onward) | same thread | `if (g_usb_up) xfer_service();` at `main.c:2865`-`2866`, unconditional under `#if SP1_XFER_ENABLE` (`#define`d `1`) |
| USB/transfer reception | `xfer_service()` (`main.c:2302`), CDC read via `cdc_rx()` | `streamer_thread()`'s loop | classic `'P'/'R'/'W'/'F'/'X'` verbs unchanged; new `'Z'` verb at `main.c:2389` |
| CRC / validation | `st_stem_validate_commit()` (`st_stem_validate.c`), called at `main.c:2487` | `'Z'` verb handler in `xfer_service()` | real per-stem CRC32 computed via `st_crc32_update()` over `emmc_read_blocks()` bursts (`main.c:2472`–`2485`), not trusted from the host |
| Storage write / commit | `meta_write_blocks()` (`main.c:1006`) called at `main.c:2525`, gated by `st_stem_validate_commit(...) == ST_STEM_OK` | `'Z'` verb handler, inside the `else` branch of `if (r != ST_STEM_OK)` | proven by `stemtape_player_safety_gate.py` pass D (§2) — a real, automated, brace-depth reachability trace, not a source-reading claim |
| Audio playback | `audio_thread()` (`main.c:3588`) → `looper_audio_block()` (`main.c:1283`) | `k_thread_create` at `main.c:3679` | all 4 tracks mixed from one shared playhead (inherited, unchanged mixing structure) |
| Fader input | `ladder_read(&adc_ladder[LAD_FADER0 + fi])` (`main.c:228`, called e.g. `main.c:5704`) | `main()`'s ~8 ms control loop | raw ADC ladder scan, unchanged from the classic looper |
| LED output | `led_service()` (`main.c:4341`), called from `main()`'s control loop (e.g. `main.c:5809`) | `main()` | drives the 8 physical LEDs directly via raw GPIO + the soft-PWM timer ISR (`led_pwm_isr`), unchanged from the classic looper |

Every function named above is linked in the real firmware ELF — see §2 for
the automated proof, not a manual claim.

## 2. ELF / link evidence

Source: real CI job logs, `stemtape-player` job, run
[32044403364](https://github.com/msakho1/stem-tape-studio/actions/runs/32044403364),
commit `28a832775d5806d49d5054c314339064d718e600`.

**"STEM TAPE runtime symbol-presence gate (fail-closed)"** step — `success`.
Checks (via `grep -qE " [tT] ${sym}\$" audit/nm.txt`, i.e. matches either
external `T` or internal-linkage `t` symbols, fails closed if `nm.txt` is
missing/empty):

```
emmc_read_blocks  emmc_write_blocks  emmc_cache_flush  meta_write_blocks
st_stem_validate_commit  st_crc32_update  streamer_thread  audio_thread
looper_audio_block  ladder_read  led_service  led_pwm_isr  main
```

All present. `trk_blk()` and `xfer_service()` are deliberately excluded
from this list — both are legitimately inlined by `-Os` (single, always-
taken call site each; `xfer_service()`'s exclusion was discovered from a
real CI failure, not anticipated, and is proven instead by its retained
caller `streamer_thread` plus the single unconditional call site at
`main.c:2865`-`2866`).

**"STRICT persistence safety gate (fail-closed)"** step (now
`stemtape_player_safety_gate.py`) — `success`. Its own pass D (a real,
automated call-site/brace-depth trace, run in that job, full text in
`audit/safety-gate.md` of the job's uploaded artifact) concludes:

> PASS — pass D: every NEW eMMC persistent-write call site is proven
> reachable only after `st_stem_validate_commit(...)` returned
> `ST_STEM_OK`; every other call site is unmodified, classic-baseline
> persistence behavior.

Concretely it found 16 real call sites of `emmc_write_blocks` /
`meta_write_blocks` / `emmc_cache_flush` in `main.c`: 14 match the
untouched classic Tape Looper baseline (`firmware/src/main.c`) call-for-
call (inherited, already-proven persistence behavior — cold-boot format-
fresh, `xfer_commit()`, the classic `'W'`-verb raw track-data staging
write, the deferred `g_meta_save_req`/`g_grid_save_req` handlers), and 2
are new (`main.c:2525`, `main.c:2527`, the `'Z'`-verb commit + its cache
flush) — both individually proven, by brace-depth range trace, to be
reachable only inside the validated-commit branch.

**Image assertions** step — `success`, 8/8 checks, including: application
origin `0x20000` exact, no bootloader/storage-partition overlap, no UICR
section/LOAD present, entry point inside the image, vector-table SP inside
RAM (`0x20035500`, 8-byte aligned), binary size `97,100 / 913,408` bytes
(10.63% of the SP-1 bootloader allowance), and the reconstructed `.bin` is
byte-identical to what `objcopy` produces from the linked `.elf`.

**RAM** (`readelf -W -S -l -h`, same job): `bss` = 201,078 B, `noinit` =
54,356 B — byte-identical to the previously-reported post-fix totals
(commit `b2f0d49`), confirming the RAM-overflow fix from earlier this
session is stable.

## 3. Deterministic 4-stem test fixture

`firmware/stemtape_player/tests/fixtures/` — generated by
`generate_fixture.py` (checked in alongside its output; re-run it to
reproduce byte-for-byte):

- **Format**: matches the REAL current on-flash representation
  (`SP1_CODEC_PCM`, `SAMP_PER_BLK` = 256 mono int16 samples / 512-byte
  eMMC block — see `main.c`'s "STORAGE CODEC TOGGLE" comment). This is
  **not** the stereo/24-bit format described as a follow-up milestone in
  `main.c`'s provenance banner (see §6, "known gap" — the banner was
  corrected this session to stop overclaiming this).
- 4 stems × 8 blocks × 512 bytes = 4,096 bytes/stem, ~42.7 ms @ 48 kHz.
- Purely algorithmic, license-safe content: stem0 = 220 Hz sine, stem1 =
  330 Hz sine, stem2 = 110 Hz sine, stem3 = xorshift32 PRNG noise (fixed
  seed) — no external audio, no copyrighted material.
- One corrupted variant (`stem0_corrupt.bin`, single flipped bit) for
  CRC-mismatch-rejection testing.
- `manifest.json` records, per stem: byte count, SHA-256, and the real
  CRC-32 (IEEE 802.3, same algorithm `st_crc32.c` implements), plus the
  exact 39-byte `'Z'`-verb payload that would commit the valid set as
  slot 0.

SHA-256 (reproduced by `sha256sum firmware/stemtape_player/tests/fixtures/*.bin`):

```
5a547d4c0bcb5acd33d5b64cfa261ca80877a8ccf8e703f5fe9232bf3f39ae92  stem0.bin
e778cb3e2b464c0ecaf3a80cd1621c64ab6cb04c45b18425e5ef9780470df08c  stem0_corrupt.bin
2f039346f018efad3fc045615d18408225508f57a363886c773256f94e884f89  stem1.bin
b2215f021de9dd9175af87ae2e934949c7efb4079738395adf408319675a9ade  stem2.bin
8f61a0703873ef6c2eed4c79d295c8b263442b0f4bb3bd15927c83150b2260ad  stem3.bin
```

## 4. Automated test matrix

`firmware/stemtape_player/tests/test_stemtape_player.c`, host-run in CI
("Stem Tape Player host tests" step, no Zephyr/nRF toolchain needed) —
**43 test cases, 168 assertion checks, all pass, zero compiler warnings**
(`cc -std=c11 -Wall -Wextra`).

| Required scenario | Covered by | How |
|---|---|---|
| CRC vectors | `test_crc32` | standard IEEE 802.3 check-vector `"123456789"` → `0xCBF43926` |
| Valid package accepted | `test_stem_validate_accepts_valid_commit`, `test_stem_validate_fixture_valid_set_accepted` | synthetic AND real fixture bytes through the real `st_stem_validate_commit()` |
| Corrupt package rejected | `test_stem_validate_rejects_crc_mismatch`, `test_stem_validate_fixture_corrupt_set_rejected` | synthetic AND real fixture bytes (one flipped bit) |
| Missing stem(s) | `test_stem_validate_rejects_missing_stem` | `present_mask` with fewer than 4 bits set |
| Truncation / size violations | `test_stem_validate_rejects_zero_length`, `test_stem_validate_rejects_oversize` | zero-length and over-capacity `frame_count` |
| Length mismatch (desync) | `test_stem_validate_rejects_length_mismatch` | one stem one block longer than the others |
| Deterministic rejection order | `test_stem_validate_check_order_is_deterministic` | multiple simultaneous violations, fixed documented precedence |
| Fixture integrity | `test_stem_validate_fixture_manifest_crc_matches_generator` | recomputes each fixture stem's CRC32 from disk, cross-checked against the manifest |

**Honest coverage boundary** (see §6): `st_stem_validate_commit()` is real,
unmodified production code, and every scenario above exercises it exactly
as `xfer_service()`'s `'Z'` verb does. What these tests do **not** and
cannot reach is `main.c` itself — `streamer_thread()`, `xfer_service()`,
`audio_thread()`, `led_service()` — because `main.c` depends on the
Zephyr kernel/USB/GPIO/I2S APIs and is Zephyr-only; it is not part of the
host-test build (see `CMakeLists.txt` and the CI `cc` command — main.c is
absent from both host-test source lists). So "duplicated stems",
"interrupted transfer", "commit-failure-preserves-previous-song",
"cold-boot recovery", "playback sync", "fader mapping", and "LED
transitions" as end-to-end *device behavior* are proven here only at the
source/call-path level (§1) plus the linked-symbol/reachability level
(§2), not by an executable test. That gap is real and is exactly what §5
covers.

## 5. Physical hardware validation procedure — **AWAITING DEVICE VALIDATION**

No physical SP-1 is available in this environment. Nothing below has been
performed; this is the procedure for whoever has the hardware.

1. Flash `stemtape_player.bin` (SHA-256 `124e9a9024b5be99193e4231205fe9a82d08131068d041d7180a21695c17fcb4`) via the SP-1's UF2/DFU path, same as the classic looper.
2. **Upload**: over USB, send the classic `'W'`-verb raw blocks for
   `firmware/stemtape_player/tests/fixtures/stem{0,1,2,3}.bin` to slot 0's
   four track regions, then send the `'Z'` verb with the exact payload in
   `manifest.json`'s `z_verb_payload_hex`. Expect response `'z'`.
3. **Persist**: confirm the device does not report an eMMC bus error
   during the commit.
4. **Power-cycle**: fully power off (hold PWR ≥3 s or pull power), then
   power back on, USB **disconnected**.
5. **Cold-boot recovery**: confirm the device boots showing slot 0
   selected and playable (LEDs indicate a loaded song, not "no song").
6. **USB-disconnected PLAY**: press PLAY; confirm all 4 stems play in
   sync (shared playhead) with no USB attached.
7. **Fader control**: move each of the 4 faders; confirm the
   corresponding stem's level responds live.
8. **LEDs**: confirm the status/track LED state matches what
   `led_service()`/`st_led_pattern.c`'s documented semantics predict for
   "playing, slot 0, no transfer, no fault" (see
   `docs/stem-tape-led-feedback-v1.md`).
9. **Corruption rejection**: repeat step 2 but substitute
   `stem0_corrupt.bin` for stem 0 while declaring the ORIGINAL
   `stem0.bin`'s CRC (i.e. don't change `z_verb_payload_hex`); expect
   response `'e'` (reject) and slot 0's previously-committed song (from
   step 2) to remain the one that plays after step 6 is repeated — this
   is the on-hardware confirmation of "commit failure preserves the
   previous song."
10. **Interrupted transfer**: start an upload, physically disconnect USB
    mid-transfer (before sending `'Z'`), then reconnect and confirm the
    device is still on the previously-committed song (never attempted a
    ‘Z' verb call, so nothing new was ever marked `present[]=1`).

None of steps 1–10 have been run. Do not treat §1/§2/§4 (source, link, CI
evidence) as a substitute for this procedure — they establish that the
code paths are real, linked, and gated correctly; they do not establish
that the physical faders, LEDs, eMMC card, or I2S/codec path behave
correctly on real hardware.

## 6. Known gaps / honest limitations

- **Payload format**: still mono 16-bit PCM per track (4 independently-
  addressed but length/CRC-synchronized tracks), not yet interleaved
  stereo 24-bit stems. `main.c`'s provenance banner previously overclaimed
  this; it was corrected this session (commit pending) to describe the
  stereo/24-bit format as a follow-up milestone, not current behavior.
- **`main.c` is not host-testable**: end-to-end behavior for cold-boot
  recovery, interrupted transfer, commit-failure preservation, playback
  sync, fader mapping and LED transitions is proven at the source/call-
  path and link level (§1–§2) but not by an executable test, because
  `main.c` requires the Zephyr kernel and is excluded from the host-test
  build. A Zephyr `native_sim` harness with mocked eMMC/ADC/GPIO/I2S
  drivers could close this gap but does not exist yet and was not
  attempted this session (out of scope for this pass).
- **Gesture/scrub/FX modules** (`st_gesture.c`, `st_scrub.c`,
  `st_led_pattern.c`, `st_storage_layout.c`, `st_sector_codec.c`,
  `st_transfer.c`) are still host-tested (pure logic, no regressions) but
  are **not compiled into the real firmware target** — they implement an
  earlier, superseded standalone protocol/runtime design. Wiring them into
  the real runtime is explicitly out of scope for this vertical-slice
  milestone.
