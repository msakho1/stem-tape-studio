/*
 * st_v11_format.h — Stem Tape transfer contract v1.1: THE single source of
 * numeric constants and byte offsets, mirroring the companion's own single
 * source of truth (src/sp1/stemTapeFormat.ts, frozen at handoff/v1.1/ —
 * see docs/stem-tape-transfer-v1.1.md section 12, "Generated numeric
 * appendix", which is itself asserted verbatim against that TypeScript file
 * by the companion's own docContract.test.ts).
 *
 * Every value below was either copied directly from that doc section or
 * independently reproduced from the frozen handoff/v1.1/ fixtures (see the
 * migration commits' own test files for the byte-level proof) -- nothing
 * here is invented.
 *
 * PURE: constants only, no I/O, no Zephyr, no dynamic allocation.
 *
 * v1.1 replaces this target's original single-header two-copy generation
 * scheme with true A/B storage (two song regions, two STIX v2 index
 * regions) and a new Q -> STCP capability reply carrying the region
 * geometry. See docs/stem-tape-transfer-v1.1.md for the full contract.
 * This header defines the WIRE FORMAT constants the migration's later
 * commits (STCP capability reply, STIX v2 index, A/B region layout) build
 * on; it does not by itself wire anything into main.c.
 */

#ifndef STEMTAPE_PLAYER_V11_FORMAT_H_
#define STEMTAPE_PLAYER_V11_FORMAT_H_

#include <stdint.h>

/* ---- transport / sector geometry (unchanged from v1.0) [doc 12.1, 12.3] */

#define ST11_PHYSICAL_BLOCK_BYTES 512u
#define ST11_BLOCKS_PER_SECTOR    16u
#define ST11_SECTOR_BYTES         (ST11_PHYSICAL_BLOCK_BYTES * ST11_BLOCKS_PER_SECTOR) /* 8192 */
#define ST11_REQUIRED_ALIGNMENT   ST11_PHYSICAL_BLOCK_BYTES

#define ST11_BAUD_RATE            115200u
/* entry magic "SP1XFER!" -- unchanged Tape Looper transport, already
 * handled by the existing xfer entry path; listed here only for parity
 * with the doc's numeric appendix. */
#define ST11_READ_ACK             0x72u
#define ST11_WRITE_ACK            0x77u
#define ST11_FLUSH_ACK            0x66u
#define ST11_CMD_CAPS             0x51u /* 'Q' */
#define ST11_CAPS_TAG_0           'S'
#define ST11_CAPS_TAG_1           'T'
#define ST11_CAPS_TAG_2           'C'
#define ST11_CAPS_TAG_3           'P'
#define ST11_CAPS_BYTES           96u
#define ST11_WRITE_RETRIES        3u

/* ---- audio geometry [doc 12.3] -------------------------------------- */

#define ST11_SAMPLE_RATE_HZ       48000u
#define ST11_STEM_COUNT           4u
#define ST11_CHANNELS_PER_STEM    2u
/*
 * ======================================================================
 * v1.3: 16-BIT STORED SAMPLES. Measured, not chosen by taste.
 * ======================================================================
 * The stems were stored at 24 bits and the I2S output has always been 16
 * (see main.c's `struct i2s_config` `.word_size = 16`), so the top 8 bits
 * of every stored sample were discarded at the last step of the mixdown.
 * They were not free: four 24-bit stereo stems at 48 kHz are 1,152,000 B/s,
 * and a hardware capture measured the streamer delivering 1,155,072 B/s
 * with 6% CPU idle -- consumption exactly matched, no surplus at all. Pitch,
 * FX and the reverse feature each need surplus, so each of them pushed the
 * transport into starvation, heard as crackle and as the song dragging.
 *
 * 16-bit storage is 768,000 B/s, a third less, and it costs essentially
 * nothing audible. Measured on the frozen four-stem fixture, converting the
 * stems to 16-bit and rendering both widths through the production mixdown:
 *
 *     residual RMS -93.5 dBFS, peak error 1 LSB, against a -6.4 dBFS mix.
 *
 * ROUND-TO-NEAREST, AND DELIBERATELY NO DITHER. Truncation is genuinely
 * wrong -- it leaves a +0.44 LSB DC bias and 2 LSB peak error -- but dither
 * measured WORSE (-89.3 dBFS) and, swept from 0 to -60 dB of source level,
 * its residual stayed flat. Dither decorrelates a FINAL quantisation; this
 * one is not final, because the mixer re-quantises to int16 after summing
 * and that undithered stage sets the floor at every level. So the companion
 * rounds and does not dither, which also removes an RNG from the encoder.
 *
 * THE DECODE GETS FASTER, not just smaller. A 16-bit stereo frame is 4
 * bytes, and the group header is 8, so every frame starts 4-byte aligned:
 * the whole stereo pair is ONE aligned word load. At 24 bits the 6-byte
 * stride alternates alignment and each sample is a three-byte assemble with
 * a sign-extend. Asserted in st_planar.h, where the decode lives.
 *
 * NOT OVERRIDABLE, DELIBERATELY. It was, briefly, so a historical harness
 * could decode the frozen 24-bit fixtures -- and the price was a second
 * sample-reading path inside st_pl_decode_stem_inline(), which is the
 * innermost loop of the 48 kHz render. v1.3 is a breaking storage migration
 * that fails closed on v1.1 and v1.2 media by design, so that path could
 * never be taken by shipped firmware; carrying it to satisfy a CI gate was
 * the wrong trade. The gate was rescoped instead --
 * docs/stem-tape-v11-conformance-retirement.md records exactly what stopped
 * being proven and what took its place. There is now ONE stored width, and
 * one way to read a sample.
 */
#define ST11_PCM_BIT_DEPTH        16u
#define ST11_BYTES_PER_SAMPLE     (ST11_PCM_BIT_DEPTH / 8u) /* 2 */

/*
 * THE WIDTH A GIVEN STORED FORMAT VERSION IMPLIES.
 *
 * The stored width is not an independent field a record may choose: it is
 * decided by the format version, and a record that disagrees with its own
 * version is malformed. Saying that once, here, is what lets st_stix.c check
 * a record's declared bit_depth against the version the record itself carries
 * rather than against this build's constant.
 *
 * For the shipped firmware the two are identical -- the version check already
 * requires format_minor == ST11_FORMAT_MINOR, so the width check that follows
 * can only pass at ST11_PCM_BIT_DEPTH. What it buys is that a test harness
 * replaying a RECORDED v1.1 session (see
 * docs/stem-tape-v11-conformance-retirement.md) can validate those records
 * without the build being handed a 24-bit width -- which is what would have
 * dragged a 24-bit decoder back into the 48 kHz path.
 */
#define ST11_BIT_DEPTH_FOR_FORMAT(minor) (((minor) >= 3u) ? 16u : 24u)
/* One frame carries all four stems, both channels: 4*2*2 = 16 bytes. */
#define ST11_BYTES_PER_FRAME      (ST11_STEM_COUNT * ST11_CHANNELS_PER_STEM * ST11_BYTES_PER_SAMPLE)
#define ST11_STEM_FRAME_BYTES     (ST11_CHANNELS_PER_STEM * ST11_BYTES_PER_SAMPLE) /* 4 */

/* Stem order, fixed (docs/stem-tape-transfer-v1.1.md section 8, matching
 * this target's existing ST_STEM_VOCAL.. constants in st_storage_layout.h). */
#define ST11_STEM_VOCAL      0u
#define ST11_STEM_DRUMS      1u
#define ST11_STEM_BASS       2u
#define ST11_STEM_INSTRUMENT 3u

/* ---- STSC sector layout [doc section 8, 12.3] ------------------------
 * ONE 32-byte header + 8160-byte LINEAR frame payload (v1.3: 510 frames * 16
 * bytes/frame; v1.1/v1.2 was 340 * 24 -- the same 8160 either way),
 * conventional per-frame ordering (stem-major, channel-major:
 * vocal L, vocal R, drums L, drums R, bass L, bass R, inst L, inst R), each
 * sample signed little-endian at ST11_PCM_BIT_DEPTH. Deliberately NOT the timknapen wiki's
 * physical-sub-block/reordered-byte-interleave format st_sector_codec.c
 * implements for the classic looper's OWN native storage -- that format
 * governs a disjoint address range this migration never touches; this is
 * the real, already-implemented-and-tested companion contract for the NEW
 * Stem Tape song-data region (see st_sector_codec.c's own doc comment,
 * which predates this migration and is now known to be the wrong format
 * for this region -- st_sector_codec.c is left untouched here and removed
 * only once this replacement is fully wired, per the migration plan). */
#define ST11_SECTOR_HEADER_BYTES  32u
#define ST11_SECTOR_PAYLOAD_BYTES (ST11_SECTOR_BYTES - ST11_SECTOR_HEADER_BYTES) /* 8160 */
#define ST11_FRAMES_PER_SECTOR    (ST11_SECTOR_PAYLOAD_BYTES / ST11_BYTES_PER_FRAME) /* 510 at 16-bit */

#define ST11_SECTOR_MAGIC 0x53545343u /* 'STSC', little-endian on the wire */

/* Byte offsets within the 32-byte sector header. All multi-byte fields are
 * little-endian (doc section 12, "All multi-byte fields are LITTLE-ENDIAN"). */
#define ST11_SECTOR_OFF_MAGIC         0u  /* u32 */
#define ST11_SECTOR_OFF_SECTOR_INDEX  4u  /* u32 */
#define ST11_SECTOR_OFF_FIRST_FRAME   8u  /* u32 */
#define ST11_SECTOR_OFF_FRAME_COUNT   12u /* u32 */
#define ST11_SECTOR_OFF_BPM_Q8        16u /* u32 */
#define ST11_SECTOR_OFF_DOWNBEAT      20u /* u32 */
#define ST11_SECTOR_OFF_LED_RESERVED  24u /* 4 bytes, firmware-owned, never invented by the companion */
#define ST11_SECTOR_OFF_RESERVED      28u /* 4 bytes */

#if !defined(__cplusplus)
_Static_assert(ST11_SECTOR_HEADER_BYTES + ST11_FRAMES_PER_SECTOR * ST11_BYTES_PER_FRAME ==
		       ST11_SECTOR_BYTES,
	       "STSC sector geometry does not tile the sector exactly");
/* ZERO PADDING AT BOTH WIDTHS, and that is what made the migration cheap
 * rather than a rewrite: 32 + 340*24 == 32 + 510*16 == 8192. The container
 * did not move; only how many frames fit inside it. The same holds for the
 * 2048-byte planar group -- see st_planar.h. */
_Static_assert(ST11_SECTOR_PAYLOAD_BYTES % ST11_BYTES_PER_FRAME == 0u,
	       "frames must tile the sector payload with no remainder");
/* The mixer reduces the stored domain to the 16-bit I2S output domain by
 * shifting right (ST11_PCM_BIT_DEPTH - 16). At 16-bit that shift is zero and
 * the samples are already at output scale; a depth BELOW 16 would make it
 * negative, which is undefined, so it is refused here rather than in the
 * 48 kHz loop. */
_Static_assert(ST11_PCM_BIT_DEPTH >= 16u,
	       "stored depth below 16 would make the mixer's output shift negative");
#endif

/* ---- versions and identity [doc 12.2] --------------------------------- */

#define ST11_FIRMWARE_ID   0x53544657u /* 'STFW' */
#define ST11_PROTOCOL_MAJOR 1u
/*
 * 1 -> 3 WITH THE 16-BIT PAYLOAD. The companion reads this out of the STCP
 * capability reply ('Q') before it uploads anything, so a companion that
 * still encodes 24-bit sees the mismatch and refuses, rather than writing a
 * song this firmware would decode as noise. That is the FIRST of the two
 * fail-closed layers; the second is the per-group one in st_planar.h, which
 * catches a song already on the card from before the migration.
 *
 * Skipping 2 is deliberate: v1.2 is the 24-bit song-planar format this
 * replaces, and reusing its number for a different payload width is exactly
 * the ambiguity these fields exist to prevent.
 *
 * OVERRIDABLE, and unlike the width this costs nothing anywhere: it is a
 * version integer that appears in one serialiser. The transcript harness
 * replays a RECORDED v1.1 session frame for frame, and the recorded 'Q' reply
 * carries the protocol version the device answered with -- so reproducing it
 * requires answering as that version. No code path is duplicated to allow it.
 *
 * The SHIPPED value is asserted in CI in a build that takes no overrides, and
 * the reply's byte-exactness is checked against
 * handoff/v1.3/binaries/stcp-capability-response.bin -- the reply this
 * firmware actually sends -- which is a stronger test than reproducing a
 * retired one.
 */
#ifndef ST11_PROTOCOL_MINOR
#define ST11_PROTOCOL_MINOR 3u
#endif
#define ST11_FORMAT_MAJOR   1u
/*
 * THE STORAGE FORMAT VERSION, AND WHY IT IS OVERRIDABLE.
 *
 * 2 = song-planar (v1.2): each stem's whole timeline contiguous in its own
 * quarter of the song region. 1 = the interleaved sector layout it replaced.
 *
 * st_stix.c refuses an index record whose format version differs from this,
 * and compatibility.ts requires the STCP reply to match the companion's, so a
 * v1.1 song is REFUSED by v1.2 firmware rather than misread -- which matters
 * more here than in most version bumps, because a v1.1 sector read as four
 * planar groups is not garbage, it is one stem's timeline played as all four.
 *
 * OVERRIDABLE FOR ONE REASON ONLY: the frozen handoff fixtures in handoff/v1.1
 * are a byte-exact record of the v1.1 CONTRACT, and testing that contract
 * means testing it at its own version. Those tests compile with
 * -DST11_FORMAT_MINOR=1u and keep proving what they always proved; the
 * shipped firmware's value is asserted separately in CI so this cannot be
 * used to quietly ship the wrong one.
 */
/*
 * 2 -> 3 WITH THE 16-BIT PAYLOAD, and this is the STRONGEST of the three
 * fail-closed layers because it rejects at song LOAD rather than per group.
 *
 * Not to be confused with ST11_PROTOCOL_MINOR above: that one versions the
 * TRANSPORT (what the companion negotiates over USB before it uploads), this
 * one versions the STORED FORMAT (what is on the card). They moved together
 * here only because v1.3 changed both.
 *
 * st_stix_validate() compares this against the index record's own
 * format_minor, so a v1.2 index still on a card returns ST_STIX_ERR_VERSION
 * and the song never loads -- before a single audio group is fetched, and
 * with a diagnostic that names the reason instead of playing 24-bit bytes as
 * 16-bit noise.
 */
#ifndef ST11_FORMAT_MINOR
#define ST11_FORMAT_MINOR   3u
#endif
#define ST11_STIX_VERSION   2u
#define ST11_INDEX_MAGIC    0x53544958u /* 'STIX', written LAST -- the sole commit point */
#define ST11_SLOT_A         0u
#define ST11_SLOT_B         1u
#define ST11_NO_SLOT        0xffffffffu

/* ---- capability flags [doc 12.4] --------------------------------------- */

#define ST11_CAP_FOUR_STEMS        (1u << 0)
#define ST11_CAP_STEREO            (1u << 1)
#define ST11_CAP_RATE_48K          (1u << 2)
#define ST11_CAP_DEPTH_24          (1u << 3)
#define ST11_CAP_INDEX_EXTENSION   (1u << 4)
#define ST11_CAP_BPM_DOWNBEAT      (1u << 5)
#define ST11_CAP_STAGING_COW       (1u << 6) /* optional, not required */
#define ST11_CAP_EXPLICIT_INIT     (1u << 7)
#define ST11_CAP_DUAL_SONG_SLOTS   (1u << 8)
#define ST11_CAP_DUAL_INDEX_SLOTS  (1u << 9)
#define ST11_CAP_GENERATION_COMMIT (1u << 10)
#define ST11_CAP_CRASH_SAFE_REPLACE (1u << 11)

#define ST11_REQUIRED_CAP_FLAGS \
	(ST11_CAP_FOUR_STEMS | ST11_CAP_STEREO | ST11_CAP_RATE_48K | ST11_CAP_DEPTH_24 | \
	 ST11_CAP_INDEX_EXTENSION | ST11_CAP_BPM_DOWNBEAT | ST11_CAP_EXPLICIT_INIT | \
	 ST11_CAP_DUAL_SONG_SLOTS | ST11_CAP_DUAL_INDEX_SLOTS | ST11_CAP_GENERATION_COMMIT | \
	 ST11_CAP_CRASH_SAFE_REPLACE) /* 0x00000fbf, matches doc 12.4 exactly */

#if !defined(__cplusplus)
_Static_assert(ST11_REQUIRED_CAP_FLAGS == 0x00000fbfu,
	       "required capability flag set does not match docs/stem-tape-transfer-v1.1.md 12.4");
#endif

/* What this firmware actually reports in its Q -> STCP reply's `flags`
 * field: every required flag PLUS the optional STAGING_COW (bit 6) --
 * verified to match handoff/v1.1/binaries/stcp-capability-response.bin's
 * real declared flags value (4095 = 0xfff = all 12 bits) exactly (see
 * tests/test_stem_v11.c). STAGING_COW is genuinely true of this design:
 * the v1.1 replacement sequence always writes the INACTIVE song/index
 * pair and never touches the active one in place -- that IS
 * copy-on-write staging at the region level. */
#define ST11_CAP_ALL_FLAGS (ST11_REQUIRED_CAP_FLAGS | ST11_CAP_STAGING_COW)

#if !defined(__cplusplus)
_Static_assert(ST11_CAP_ALL_FLAGS == 0x00000fffu,
	       "reported capability flag set does not match the real stcp-capability-response.bin fixture");
#endif

/* ---- STCP capability record offsets [doc 12.5] ------------------------
 * Reply to 'Q': 4-byte ASCII tag "STCP" followed by this 96-byte payload
 * (100 bytes total on the wire). */
#define ST11_CAPS_OFF_FIRMWARE_ID      0u  /* u32 */
#define ST11_CAPS_OFF_PROTO_MAJOR      4u  /* u16 */
#define ST11_CAPS_OFF_PROTO_MINOR      6u  /* u16 */
#define ST11_CAPS_OFF_FORMAT_MAJOR     8u  /* u16 */
#define ST11_CAPS_OFF_FORMAT_MINOR     10u /* u16 */
#define ST11_CAPS_OFF_FLAGS            12u /* u32 */
#define ST11_CAPS_OFF_SAMPLE_RATE      16u /* u32 */
#define ST11_CAPS_OFF_BLOCK_SIZE       20u /* u32 */
#define ST11_CAPS_OFF_SECTOR_BYTES     24u /* u32 */
#define ST11_CAPS_OFF_ALIGNMENT        28u /* u32 */
#define ST11_CAPS_OFF_DEVICE_BLOCKS    32u /* u32 */
#define ST11_CAPS_OFF_SONG_A_START     36u /* u32 */
#define ST11_CAPS_OFF_SONG_A_BLOCKS    40u /* u32 */
#define ST11_CAPS_OFF_SONG_B_START     44u /* u32 */
#define ST11_CAPS_OFF_SONG_B_BLOCKS    48u /* u32 */
#define ST11_CAPS_OFF_INDEX_A_START    52u /* u32 */
#define ST11_CAPS_OFF_INDEX_A_BLOCKS   56u /* u32 */
#define ST11_CAPS_OFF_INDEX_B_START    60u /* u32 */
#define ST11_CAPS_OFF_INDEX_B_BLOCKS   64u /* u32 */
#define ST11_CAPS_OFF_ACTIVE_INDEX     68u /* u32 -- 0=A, 1=B, 0xffffffff=none */
#define ST11_CAPS_OFF_ACTIVE_SONG      72u /* u32 -- 0=A, 1=B, 0xffffffff=none */
#define ST11_CAPS_OFF_ACTIVE_GEN_LO    76u /* u32 */
#define ST11_CAPS_OFF_ACTIVE_GEN_HI    80u /* u32 */
#define ST11_CAPS_OFF_STIX_VERSION     84u /* u16 */
#define ST11_CAPS_OFF_RESERVED         86u /* 10 bytes, must be zero */

#if !defined(__cplusplus)
_Static_assert(ST11_CAPS_OFF_RESERVED + 10u == ST11_CAPS_BYTES,
	       "STCP capability record does not total 96 bytes");
#endif

/* ---- STIX v2 index record offsets [doc 12.6] --------------------------
 * One record, 256 bytes, fits inside a single 512-byte physical block so
 * the magic-last write is one atomic block write; bytes [256,512) of the
 * containing block must be zero. */
#define ST11_INDEX_RECORD_BYTES 256u
#define ST11_INDEX_TEXT_BYTES   60u

#define ST11_IX_OFF_MAGIC            0u   /* u32 -- 0 while uncommitted, ST11_INDEX_MAGIC once committed */
#define ST11_IX_OFF_INDEX_VERSION    4u   /* u16 */
#define ST11_IX_OFF_FORMAT_MAJOR     6u   /* u16 */
#define ST11_IX_OFF_FORMAT_MINOR     8u   /* u16 */
#define ST11_IX_OFF_SLOT_IDENTITY    10u  /* u8 -- 0=A, 1=B; must equal the region it was read from */
#define ST11_IX_OFF_SONG_SLOT        11u  /* u8 -- 0=A, 1=B; the song-data region this index describes */
#define ST11_IX_OFF_FLAGS            12u  /* u16 -- bit0 SONG_PRESENT */
#define ST11_IX_OFF_RESERVED0        14u  /* u16 */
#define ST11_IX_OFF_GENERATION_LO    16u  /* u32 */
#define ST11_IX_OFF_GENERATION_HI    20u  /* u32 */
#define ST11_IX_OFF_SONG_START_BLOCK 24u  /* u32 */
#define ST11_IX_OFF_SONG_BLOCK_COUNT 28u  /* u32 */
#define ST11_IX_OFF_FRAMES           32u  /* u32 */
#define ST11_IX_OFF_SECTOR_COUNT     36u  /* u32 */
#define ST11_IX_OFF_SAMPLE_RATE      40u  /* u32 */
#define ST11_IX_OFF_CHANNELS         44u  /* u16 */
#define ST11_IX_OFF_BIT_DEPTH        46u  /* u16 */
#define ST11_IX_OFF_BPM_Q8           48u  /* u32 */
#define ST11_IX_OFF_DOWNBEAT_FRAME   52u  /* u32 */
#define ST11_IX_OFF_ORIGINAL_FRAMES  56u  /* u32 * 4 (one per stem) */
#define ST11_IX_OFF_STEM_CHECKSUMS   72u  /* u32 * 4 (one per stem, FNV-1a -- see st_checksum32.h) */
#define ST11_IX_OFF_SONG_CHECKSUM    88u  /* u32 (FNV-1a over the 16-byte per-stem-checksum digest) */
#define ST11_IX_OFF_TITLE            92u  /* 60 bytes, UTF-8, NUL-padded */
#define ST11_IX_OFF_ARTIST           152u /* 60 bytes, UTF-8, NUL-padded */
#define ST11_IX_OFF_RESERVED1        212u /* 40 bytes, must be zero */
#define ST11_IX_OFF_CRC32            252u /* u32 -- record's OWN integrity CRC, last field */

#define ST11_IX_FLAG_SONG_PRESENT (1u << 0)

#if !defined(__cplusplus)
_Static_assert(ST11_IX_OFF_CRC32 + 4u == ST11_INDEX_RECORD_BYTES,
	       "STIX v2 index record does not total 256 bytes");
#endif

/*
 * CRC coverage for the record's own integrity field (offset 252), stated
 * explicitly so no implementation has to guess -- verified byte-for-byte
 * against handoff/v1.1/binaries/index-a-valid.bin's real crc32 field:
 *   crc32 = CRC-32 IEEE 802.3 (st_crc32.c) over record[0 .. 252) with
 *   record[0 .. 4) (the validity magic) NORMALIZED TO 0x00000000 during
 *   the calculation. The CRC field itself (252..256) is excluded.
 *
 * This is DIFFERENT from stemChecksums/songChecksum (offsets 72/88, DATA
 * fields inside the record): those use FNV-1a (st_checksum32.h), not
 * CRC-32 -- verified byte-for-byte against handoff/v1.1/binaries/
 * song-sectors-four-stem.bin's declared per-stem and song checksums. Do
 * not conflate the two: CRC-32 validates the record's own bytes; FNV-1a
 * validates the audio content the record describes.
 */
#define ST11_IX_CRC_RANGE_FROM 0u
#define ST11_IX_CRC_RANGE_TO   ST11_IX_OFF_CRC32
#define ST11_IX_CRC_ZEROED_FROM ST11_IX_OFF_MAGIC
#define ST11_IX_CRC_ZEROED_TO   (ST11_IX_OFF_MAGIC + 4u)

#endif /* STEMTAPE_PLAYER_V11_FORMAT_H_ */
