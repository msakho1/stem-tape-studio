/*
 * st_stcp.h — v1.1 A/B region layout + the Q -> STCP capability reply
 * (docs/stem-tape-transfer-v1.1.md sections 2, 10.1-10.2, 12.5).
 *
 * Region geometry: two 1-block STIX v2 index regions (A then B, right at
 * the storage base) followed by two equal-sized song regions, sector-
 * aligned, splitting whatever device capacity remains evenly in half --
 * verified to reproduce handoff/v1.1/binaries/stcp-capability-response.bin's
 * real region descriptors EXACTLY for that fixture's 272-block synthetic
 * test device (see tests/test_stem_v11.c): index A=[0,1), index B=[1,1),
 * song A=[16,144), song B=[144,272). This is a genuinely capacity-
 * adaptive layout (unlike st_storage_layout.h's v1.0 fixed
 * ST_MAX_SONG_SECONDS ceiling) -- deliberately so, since v1.1 needs BOTH
 * song regions to independently hold a full song at once, and the real
 * device's total capacity is only known at runtime.
 *
 * `base_block` lets the pure layout math be verified directly against the
 * fixture (whose synthetic mock device numbers its own blocks from 0);
 * real firmware wiring (a later migration commit) passes
 * ST_STORAGE_BASE_BLOCK (st_storage_layout.h) as the anchor, so the v1.1
 * region never overlaps the pre-existing library-header/staging/song-data
 * layout OR the classic looper's own disjoint region.
 *
 * PURE: no I/O, no Zephyr.
 */

#ifndef STEMTAPE_PLAYER_STCP_H_
#define STEMTAPE_PLAYER_STCP_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_v11_format.h"

#define ST11_INDEX_REGION_BLOCKS 1u /* one physical 512-byte block per STIX v2 record */

typedef struct {
	uint32_t index_a_start;
	uint32_t index_a_blocks;
	uint32_t index_b_start;
	uint32_t index_b_blocks;
	uint32_t song_a_start;
	uint32_t song_a_blocks;
	uint32_t song_b_start;
	uint32_t song_b_blocks;
} st11_region_layout_t;

/*
 * Computes the A/B region layout for a device with `device_blocks_total`
 * blocks available starting at `base_block` (all returned starts are
 * absolute: base_block + relative offset). Index A/B take one block each,
 * immediately at the base; song A/B split the remainder evenly, sector-
 * aligned (whole ST11_BLOCKS_PER_SECTOR chunks only -- st_v11_format.h's
 * "song regions in whole 8 KiB sectors only" rule), starting right after
 * the index regions rounded UP to the next sector boundary.
 *
 * Returns false (out left zeroed) if there is not enough room for at
 * least one sector per song region -- fails closed rather than reporting
 * a capability structure the device cannot actually honor.
 */
bool st11_storage_layout_compute(uint32_t base_block, uint32_t device_blocks_total,
				  st11_region_layout_t *out);

/*
 * Builds the exact ST11_CAPS_TAG_BYTES(4) + ST11_CAPS_BYTES(96) = 100-byte
 * wire reply to 'Q': ASCII tag "STCP" followed by the little-endian
 * capability payload (docs 12.5). Every fixed identity/format/flags field
 * (firmwareId, protocol/format major.minor, REQUIRED capability flags,
 * sampleRate, blockSize, sectorBytes, alignment, stixVersion) comes from
 * st_v11_format.h -- never passed in, so a caller cannot accidentally
 * advertise a wrong/inconsistent value. `active_index_slot`/
 * `active_song_slot` should be ST11_SLOT_A/ST11_SLOT_B or ST11_NO_SLOT
 * (0xffffffff) if no valid index has ever been committed;
 * `active_generation` is the 64-bit generation of the currently selected
 * STIX record (0 if none). Reserved bytes [86,96) are always zero.
 */
void st11_stcp_build(const st11_region_layout_t *layout, uint32_t device_blocks_total,
		      uint32_t active_index_slot, uint32_t active_song_slot,
		      uint64_t active_generation, uint8_t out[4 + ST11_CAPS_BYTES]);

typedef struct {
	uint32_t firmware_id;
	uint16_t proto_major;
	uint16_t proto_minor;
	uint16_t format_major;
	uint16_t format_minor;
	uint32_t flags;
	uint32_t sample_rate;
	uint32_t block_size;
	uint32_t sector_bytes;
	uint32_t alignment;
	uint32_t device_blocks;
	st11_region_layout_t regions;
	uint32_t active_index_slot;
	uint32_t active_song_slot;
	uint32_t active_generation_lo;
	uint32_t active_generation_hi;
	uint16_t stix_version;
} st11_stcp_reply_t;

/*
 * Parses a received "STCP" + 96-byte payload (100 bytes total) into its
 * fields, for a companion-side reader or (as used in tests/test_stem_v11.c)
 * to prove st11_stcp_build()'s output round-trips exactly against the
 * real handoff/v1.1/binaries/stcp-capability-response.bin fixture. Returns
 * false (out unmodified) if the leading 4-byte tag is not "STCP".
 */
bool st11_stcp_parse(const uint8_t in[4 + ST11_CAPS_BYTES], st11_stcp_reply_t *out);

typedef enum {
	ST11_REGION_INDEX_A,
	ST11_REGION_INDEX_B,
	ST11_REGION_SONG_A,
	ST11_REGION_SONG_B,
	ST11_REGION_NONE, /* outside every permitted v1.1 region -- must never be written */
} st11_region_id_t;

/*
 * THE bounds gate a real 'W'/'R' handler must call before ever touching
 * eMMC (docs section 1: the wire transport is the unchanged classic Tape
 * Looper 'W'/'R'/'F' -- there is no new staging verb in v1.1; the
 * companion does ALL retry/verify/ordering logic on its own side and just
 * issues raw block writes, so bounds-checking against the real,
 * capability-reported region layout is the ENTIRE firmware-side safety
 * boundary for this contract. Fails closed: any block not inside exactly
 * one of the four regions in `layout` is ST11_REGION_NONE, including gaps
 * between regions (e.g. between the index regions and the sector-aligned
 * start of song A) and anything past the last song region.
 */
st11_region_id_t st11_region_of_block(const st11_region_layout_t *layout, uint32_t block);

#endif /* STEMTAPE_PLAYER_STCP_H_ */
