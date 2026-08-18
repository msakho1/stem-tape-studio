/*
 * st_stix.c — see st_stix.h. PURE.
 */

#include "st_stix.h"

#include <string.h>

#include "st_crc32.h"
#include "st_transfer_protocol.h" /* ST_CRC32_INIT */

static void put_u32le(uint8_t *out, uint32_t off, uint32_t v)
{
	out[off + 0] = (uint8_t)(v & 0xffu);
	out[off + 1] = (uint8_t)((v >> 8) & 0xffu);
	out[off + 2] = (uint8_t)((v >> 16) & 0xffu);
	out[off + 3] = (uint8_t)((v >> 24) & 0xffu);
}

static uint32_t get_u32le(const uint8_t *in, uint32_t off)
{
	return (uint32_t)in[off + 0] | ((uint32_t)in[off + 1] << 8) |
	       ((uint32_t)in[off + 2] << 16) | ((uint32_t)in[off + 3] << 24);
}

static void put_u16le(uint8_t *out, uint32_t off, uint16_t v)
{
	out[off + 0] = (uint8_t)(v & 0xffu);
	out[off + 1] = (uint8_t)((v >> 8) & 0xffu);
}

static uint16_t get_u16le(const uint8_t *in, uint32_t off)
{
	return (uint16_t)((uint32_t)in[off + 0] | ((uint32_t)in[off + 1] << 8));
}

void st_stix_deserialize(const uint8_t in[ST11_PHYSICAL_BLOCK_BYTES], st_stix_record_t *out)
{
	uint32_t i;

	out->magic = get_u32le(in, ST11_IX_OFF_MAGIC);
	out->index_version = get_u16le(in, ST11_IX_OFF_INDEX_VERSION);
	out->format_major = get_u16le(in, ST11_IX_OFF_FORMAT_MAJOR);
	out->format_minor = get_u16le(in, ST11_IX_OFF_FORMAT_MINOR);
	out->slot_identity = in[ST11_IX_OFF_SLOT_IDENTITY];
	out->song_slot = in[ST11_IX_OFF_SONG_SLOT];
	out->flags = get_u16le(in, ST11_IX_OFF_FLAGS);
	out->generation_lo = get_u32le(in, ST11_IX_OFF_GENERATION_LO);
	out->generation_hi = get_u32le(in, ST11_IX_OFF_GENERATION_HI);
	out->song_start_block = get_u32le(in, ST11_IX_OFF_SONG_START_BLOCK);
	out->song_block_count = get_u32le(in, ST11_IX_OFF_SONG_BLOCK_COUNT);
	out->frames = get_u32le(in, ST11_IX_OFF_FRAMES);
	out->sector_count = get_u32le(in, ST11_IX_OFF_SECTOR_COUNT);
	out->sample_rate = get_u32le(in, ST11_IX_OFF_SAMPLE_RATE);
	out->channels = get_u16le(in, ST11_IX_OFF_CHANNELS);
	out->bit_depth = get_u16le(in, ST11_IX_OFF_BIT_DEPTH);
	out->bpm_q8 = get_u32le(in, ST11_IX_OFF_BPM_Q8);
	out->downbeat_frame = get_u32le(in, ST11_IX_OFF_DOWNBEAT_FRAME);
	for (i = 0; i < ST11_STEM_COUNT; i++) {
		out->original_frames[i] = get_u32le(in, ST11_IX_OFF_ORIGINAL_FRAMES + i * 4u);
		out->stem_checksums[i] = get_u32le(in, ST11_IX_OFF_STEM_CHECKSUMS + i * 4u);
	}
	out->song_checksum = get_u32le(in, ST11_IX_OFF_SONG_CHECKSUM);
	memcpy(out->title, in + ST11_IX_OFF_TITLE, ST11_INDEX_TEXT_BYTES);
	memcpy(out->artist, in + ST11_IX_OFF_ARTIST, ST11_INDEX_TEXT_BYTES);
	out->crc32 = get_u32le(in, ST11_IX_OFF_CRC32);
}

void st_stix_serialize(const st_stix_record_t *r, uint8_t out[ST11_PHYSICAL_BLOCK_BYTES])
{
	uint32_t i;

	memset(out, 0, ST11_PHYSICAL_BLOCK_BYTES);

	put_u32le(out, ST11_IX_OFF_MAGIC, r->magic);
	put_u16le(out, ST11_IX_OFF_INDEX_VERSION, r->index_version);
	put_u16le(out, ST11_IX_OFF_FORMAT_MAJOR, r->format_major);
	put_u16le(out, ST11_IX_OFF_FORMAT_MINOR, r->format_minor);
	out[ST11_IX_OFF_SLOT_IDENTITY] = r->slot_identity;
	out[ST11_IX_OFF_SONG_SLOT] = r->song_slot;
	put_u16le(out, ST11_IX_OFF_FLAGS, r->flags);
	/* reserved0 [14,16) stays zero */
	put_u32le(out, ST11_IX_OFF_GENERATION_LO, r->generation_lo);
	put_u32le(out, ST11_IX_OFF_GENERATION_HI, r->generation_hi);
	put_u32le(out, ST11_IX_OFF_SONG_START_BLOCK, r->song_start_block);
	put_u32le(out, ST11_IX_OFF_SONG_BLOCK_COUNT, r->song_block_count);
	put_u32le(out, ST11_IX_OFF_FRAMES, r->frames);
	put_u32le(out, ST11_IX_OFF_SECTOR_COUNT, r->sector_count);
	put_u32le(out, ST11_IX_OFF_SAMPLE_RATE, r->sample_rate);
	put_u16le(out, ST11_IX_OFF_CHANNELS, r->channels);
	put_u16le(out, ST11_IX_OFF_BIT_DEPTH, r->bit_depth);
	put_u32le(out, ST11_IX_OFF_BPM_Q8, r->bpm_q8);
	put_u32le(out, ST11_IX_OFF_DOWNBEAT_FRAME, r->downbeat_frame);
	for (i = 0; i < ST11_STEM_COUNT; i++) {
		put_u32le(out, ST11_IX_OFF_ORIGINAL_FRAMES + i * 4u, r->original_frames[i]);
		put_u32le(out, ST11_IX_OFF_STEM_CHECKSUMS + i * 4u, r->stem_checksums[i]);
	}
	put_u32le(out, ST11_IX_OFF_SONG_CHECKSUM, r->song_checksum);
	memcpy(out + ST11_IX_OFF_TITLE, r->title, ST11_INDEX_TEXT_BYTES);
	memcpy(out + ST11_IX_OFF_ARTIST, r->artist, ST11_INDEX_TEXT_BYTES);
	/* reserved1 [212,252) stays zero */
	put_u32le(out, ST11_IX_OFF_CRC32, r->crc32);
	/* bytes [256,512) stay zero */
}

uint32_t st_stix_block_crc(const uint8_t block[ST11_PHYSICAL_BLOCK_BYTES])
{
	static const uint8_t zero4[4] = { 0, 0, 0, 0 };
	uint32_t running = st_crc32_update(ST_CRC32_INIT, zero4, sizeof(zero4));

	running = st_crc32_update(running, block + 4, ST11_IX_CRC_RANGE_TO - 4u);
	return running ^ 0xFFFFFFFFu;
}

static bool title_bytes_valid(const uint8_t *bytes, uint32_t len)
{
	/* NUL-padded UTF-8: once a NUL is seen, every remaining byte must
	 * also be NUL (no garbage after the terminator). A field that uses
	 * all `len` bytes with no NUL at all is legal too. */
	uint32_t i;
	bool seen_nul = false;

	for (i = 0; i < len; i++) {
		if (seen_nul && bytes[i] != 0) {
			return false;
		}
		if (bytes[i] == 0) {
			seen_nul = true;
		}
	}
	return true;
}

st_stix_validity_t st_stix_validate(const uint8_t block[ST11_PHYSICAL_BLOCK_BYTES],
				     uint8_t expected_slot_identity, uint32_t song_a_start,
				     uint32_t song_a_blocks, uint32_t song_b_start,
				     uint32_t song_b_blocks, st_stix_record_t *record_out)
{
	st_stix_deserialize(block, record_out);

	if (record_out->magic != ST11_INDEX_MAGIC) {
		return ST_STIX_ERR_MAGIC;
	}
	if (st_stix_block_crc(block) != record_out->crc32) {
		return ST_STIX_ERR_CRC;
	}
	if (record_out->index_version != ST11_STIX_VERSION ||
	    record_out->format_major != ST11_FORMAT_MAJOR ||
	    record_out->format_minor != ST11_FORMAT_MINOR) {
		return ST_STIX_ERR_VERSION;
	}
	if (record_out->slot_identity != expected_slot_identity) {
		return ST_STIX_ERR_SLOT_IDENTITY;
	}

	bool present = (record_out->flags & ST11_IX_FLAG_SONG_PRESENT) != 0u;

	if (present) {
		uint32_t expect_sectors =
			(record_out->frames + ST11_FRAMES_PER_SECTOR - 1u) / ST11_FRAMES_PER_SECTOR;

		if (record_out->frames == 0u || record_out->sector_count != expect_sectors ||
		    record_out->song_block_count != record_out->sector_count * ST11_BLOCKS_PER_SECTOR ||
		    record_out->sample_rate != ST11_SAMPLE_RATE_HZ ||
		    record_out->channels != ST11_CHANNELS_PER_STEM ||
		    record_out->bit_depth != ST11_PCM_BIT_DEPTH) {
			return ST_STIX_ERR_SONG_METADATA;
		}
	} else {
		if (record_out->frames != 0u || record_out->sector_count != 0u ||
		    record_out->song_block_count != 0u || record_out->song_start_block != 0u) {
			return ST_STIX_ERR_SONG_METADATA;
		}
	}
	if (!title_bytes_valid((const uint8_t *)record_out->title, ST11_INDEX_TEXT_BYTES) ||
	    !title_bytes_valid((const uint8_t *)record_out->artist, ST11_INDEX_TEXT_BYTES)) {
		return ST_STIX_ERR_SONG_METADATA;
	}

	if (record_out->song_slot != ST11_SLOT_A && record_out->song_slot != ST11_SLOT_B) {
		return ST_STIX_ERR_BOUNDS;
	}
	if (present) {
		uint32_t region_start = (record_out->song_slot == ST11_SLOT_A) ? song_a_start : song_b_start;
		uint32_t region_blocks = (record_out->song_slot == ST11_SLOT_A) ? song_a_blocks : song_b_blocks;
		uint64_t region_end = (uint64_t)region_start + region_blocks;
		uint64_t song_end = (uint64_t)record_out->song_start_block + record_out->song_block_count;

		if (record_out->song_start_block % ST11_BLOCKS_PER_SECTOR != 0u ||
		    record_out->song_start_block < region_start || song_end > region_end) {
			return ST_STIX_ERR_BOUNDS;
		}
	}

	return ST_STIX_VALID;
}

static uint64_t generation64(const st_stix_record_t *r)
{
	return ((uint64_t)r->generation_hi << 32) | (uint64_t)r->generation_lo;
}

st_stix_select_t st_stix_select_active(const uint8_t block_a[ST11_PHYSICAL_BLOCK_BYTES],
					const uint8_t block_b[ST11_PHYSICAL_BLOCK_BYTES],
					uint32_t song_a_start, uint32_t song_a_blocks,
					uint32_t song_b_start, uint32_t song_b_blocks,
					st_stix_record_t *selected_out)
{
	st_stix_record_t rec_a;
	st_stix_record_t rec_b;
	bool valid_a = st_stix_validate(block_a, ST11_SLOT_A, song_a_start, song_a_blocks, song_b_start,
					 song_b_blocks, &rec_a) == ST_STIX_VALID;
	bool valid_b = st_stix_validate(block_b, ST11_SLOT_B, song_a_start, song_a_blocks, song_b_start,
					 song_b_blocks, &rec_b) == ST_STIX_VALID;

	if (valid_a && !valid_b) {
		*selected_out = rec_a;
		return ST_STIX_SELECT_A;
	}
	if (valid_b && !valid_a) {
		*selected_out = rec_b;
		return ST_STIX_SELECT_B;
	}
	if (valid_a && valid_b) {
		if (generation64(&rec_b) > generation64(&rec_a)) {
			*selected_out = rec_b;
			return ST_STIX_SELECT_B;
		}
		*selected_out = rec_a; /* strictly greater required for B to win; tie -> A */
		return ST_STIX_SELECT_A;
	}
	return ST_STIX_SELECT_NONE;
}
