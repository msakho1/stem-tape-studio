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
#define ST11_PCM_BIT_DEPTH        24u
#define ST11_BYTES_PER_SAMPLE     (ST11_PCM_BIT_DEPTH / 8u) /* 3 */
/* One frame carries all four stems, both channels: 4*2*3 = 24 bytes. */
#define ST11_BYTES_PER_FRAME      (ST11_STEM_COUNT * ST11_CHANNELS_PER_STEM * ST11_BYTES_PER_SAMPLE)
#define ST11_STEM_FRAME_BYTES     (ST11_CHANNELS_PER_STEM * ST11_BYTES_PER_SAMPLE) /* 6 */

/* Stem order, fixed (docs/stem-tape-transfer-v1.1.md section 8, matching
 * this target's existing ST_STEM_VOCAL.. constants in st_storage_layout.h). */
#define ST11_STEM_VOCAL      0u
#define ST11_STEM_DRUMS      1u
#define ST11_STEM_BASS       2u
#define ST11_STEM_INSTRUMENT 3u

/* ---- STSC sector layout [doc section 8, 12.3] ------------------------
 * ONE 32-byte header + 8160-byte LINEAR frame payload (340 frames * 24
 * bytes/frame), conventional per-frame ordering (stem-major, channel-major:
 * vocal L, vocal R, drums L, drums R, bass L, bass R, inst L, inst R), each
 * sample signed 24-bit little-endian. Deliberately NOT the timknapen wiki's
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
#define ST11_FRAMES_PER_SECTOR    (ST11_SECTOR_PAYLOAD_BYTES / ST11_BYTES_PER_FRAME) /* 340 */

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
#endif

/* ---- versions and identity [doc 12.2] --------------------------------- */

#define ST11_FIRMWARE_ID   0x53544657u /* 'STFW' */
#define ST11_PROTOCOL_MAJOR 1u
#define ST11_PROTOCOL_MINOR 1u
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
#ifndef ST11_FORMAT_MINOR
#define ST11_FORMAT_MINOR   2u
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
