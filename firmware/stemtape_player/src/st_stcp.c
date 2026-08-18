/*
 * st_stcp.c — see st_stcp.h. PURE.
 */

#include "st_stcp.h"

#include <string.h>

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

bool st11_storage_layout_compute(uint32_t base_block, uint32_t device_blocks_total,
				  st11_region_layout_t *out)
{
	memset(out, 0, sizeof(*out));

	out->index_a_start = base_block + 0u;
	out->index_a_blocks = ST11_INDEX_REGION_BLOCKS;
	out->index_b_start = base_block + ST11_INDEX_REGION_BLOCKS;
	out->index_b_blocks = ST11_INDEX_REGION_BLOCKS;

	uint32_t after_index = out->index_b_start + out->index_b_blocks; /* relative to block 0, but
									     * base_block already folded
									     * into index_b_start */
	uint32_t aligned_start =
		((after_index + ST11_BLOCKS_PER_SECTOR - 1u) / ST11_BLOCKS_PER_SECTOR) * ST11_BLOCKS_PER_SECTOR;
	uint32_t device_end = base_block + device_blocks_total;

	if (aligned_start >= device_end) {
		return false;
	}

	uint32_t remaining_blocks = device_end - aligned_start;
	uint32_t remaining_sectors = remaining_blocks / ST11_BLOCKS_PER_SECTOR;
	uint32_t per_region_sectors = remaining_sectors / 2u;

	if (per_region_sectors == 0u) {
		memset(out, 0, sizeof(*out));
		return false;
	}

	out->song_a_start = aligned_start;
	out->song_a_blocks = per_region_sectors * ST11_BLOCKS_PER_SECTOR;
	out->song_b_start = out->song_a_start + out->song_a_blocks;
	out->song_b_blocks = per_region_sectors * ST11_BLOCKS_PER_SECTOR;
	return true;
}

void st11_stcp_build(const st11_region_layout_t *layout, uint32_t device_blocks_total,
		      uint32_t active_index_slot, uint32_t active_song_slot,
		      uint64_t active_generation, uint8_t out[4 + ST11_CAPS_BYTES])
{
	memset(out, 0, 4 + ST11_CAPS_BYTES);
	out[0] = (uint8_t)ST11_CAPS_TAG_0;
	out[1] = (uint8_t)ST11_CAPS_TAG_1;
	out[2] = (uint8_t)ST11_CAPS_TAG_2;
	out[3] = (uint8_t)ST11_CAPS_TAG_3;

	uint8_t *p = out + 4;

	put_u32le(p, ST11_CAPS_OFF_FIRMWARE_ID, ST11_FIRMWARE_ID);
	put_u16le(p, ST11_CAPS_OFF_PROTO_MAJOR, (uint16_t)ST11_PROTOCOL_MAJOR);
	put_u16le(p, ST11_CAPS_OFF_PROTO_MINOR, (uint16_t)ST11_PROTOCOL_MINOR);
	put_u16le(p, ST11_CAPS_OFF_FORMAT_MAJOR, (uint16_t)ST11_FORMAT_MAJOR);
	put_u16le(p, ST11_CAPS_OFF_FORMAT_MINOR, (uint16_t)ST11_FORMAT_MINOR);
	put_u32le(p, ST11_CAPS_OFF_FLAGS, ST11_CAP_ALL_FLAGS);
	put_u32le(p, ST11_CAPS_OFF_SAMPLE_RATE, ST11_SAMPLE_RATE_HZ);
	put_u32le(p, ST11_CAPS_OFF_BLOCK_SIZE, ST11_PHYSICAL_BLOCK_BYTES);
	put_u32le(p, ST11_CAPS_OFF_SECTOR_BYTES, ST11_SECTOR_BYTES);
	put_u32le(p, ST11_CAPS_OFF_ALIGNMENT, ST11_REQUIRED_ALIGNMENT);
	put_u32le(p, ST11_CAPS_OFF_DEVICE_BLOCKS, device_blocks_total);
	put_u32le(p, ST11_CAPS_OFF_SONG_A_START, layout->song_a_start);
	put_u32le(p, ST11_CAPS_OFF_SONG_A_BLOCKS, layout->song_a_blocks);
	put_u32le(p, ST11_CAPS_OFF_SONG_B_START, layout->song_b_start);
	put_u32le(p, ST11_CAPS_OFF_SONG_B_BLOCKS, layout->song_b_blocks);
	put_u32le(p, ST11_CAPS_OFF_INDEX_A_START, layout->index_a_start);
	put_u32le(p, ST11_CAPS_OFF_INDEX_A_BLOCKS, layout->index_a_blocks);
	put_u32le(p, ST11_CAPS_OFF_INDEX_B_START, layout->index_b_start);
	put_u32le(p, ST11_CAPS_OFF_INDEX_B_BLOCKS, layout->index_b_blocks);
	put_u32le(p, ST11_CAPS_OFF_ACTIVE_INDEX, active_index_slot);
	put_u32le(p, ST11_CAPS_OFF_ACTIVE_SONG, active_song_slot);
	put_u32le(p, ST11_CAPS_OFF_ACTIVE_GEN_LO, (uint32_t)(active_generation & 0xFFFFFFFFu));
	put_u32le(p, ST11_CAPS_OFF_ACTIVE_GEN_HI, (uint32_t)(active_generation >> 32));
	put_u16le(p, ST11_CAPS_OFF_STIX_VERSION, (uint16_t)ST11_STIX_VERSION);
	/* reserved [86,96) stays zero */
}

bool st11_stcp_parse(const uint8_t in[4 + ST11_CAPS_BYTES], st11_stcp_reply_t *out)
{
	if (in[0] != (uint8_t)ST11_CAPS_TAG_0 || in[1] != (uint8_t)ST11_CAPS_TAG_1 ||
	    in[2] != (uint8_t)ST11_CAPS_TAG_2 || in[3] != (uint8_t)ST11_CAPS_TAG_3) {
		return false;
	}

	const uint8_t *p = in + 4;

	out->firmware_id = get_u32le(p, ST11_CAPS_OFF_FIRMWARE_ID);
	out->proto_major = get_u16le(p, ST11_CAPS_OFF_PROTO_MAJOR);
	out->proto_minor = get_u16le(p, ST11_CAPS_OFF_PROTO_MINOR);
	out->format_major = get_u16le(p, ST11_CAPS_OFF_FORMAT_MAJOR);
	out->format_minor = get_u16le(p, ST11_CAPS_OFF_FORMAT_MINOR);
	out->flags = get_u32le(p, ST11_CAPS_OFF_FLAGS);
	out->sample_rate = get_u32le(p, ST11_CAPS_OFF_SAMPLE_RATE);
	out->block_size = get_u32le(p, ST11_CAPS_OFF_BLOCK_SIZE);
	out->sector_bytes = get_u32le(p, ST11_CAPS_OFF_SECTOR_BYTES);
	out->alignment = get_u32le(p, ST11_CAPS_OFF_ALIGNMENT);
	out->device_blocks = get_u32le(p, ST11_CAPS_OFF_DEVICE_BLOCKS);
	out->regions.song_a_start = get_u32le(p, ST11_CAPS_OFF_SONG_A_START);
	out->regions.song_a_blocks = get_u32le(p, ST11_CAPS_OFF_SONG_A_BLOCKS);
	out->regions.song_b_start = get_u32le(p, ST11_CAPS_OFF_SONG_B_START);
	out->regions.song_b_blocks = get_u32le(p, ST11_CAPS_OFF_SONG_B_BLOCKS);
	out->regions.index_a_start = get_u32le(p, ST11_CAPS_OFF_INDEX_A_START);
	out->regions.index_a_blocks = get_u32le(p, ST11_CAPS_OFF_INDEX_A_BLOCKS);
	out->regions.index_b_start = get_u32le(p, ST11_CAPS_OFF_INDEX_B_START);
	out->regions.index_b_blocks = get_u32le(p, ST11_CAPS_OFF_INDEX_B_BLOCKS);
	out->active_index_slot = get_u32le(p, ST11_CAPS_OFF_ACTIVE_INDEX);
	out->active_song_slot = get_u32le(p, ST11_CAPS_OFF_ACTIVE_SONG);
	out->active_generation_lo = get_u32le(p, ST11_CAPS_OFF_ACTIVE_GEN_LO);
	out->active_generation_hi = get_u32le(p, ST11_CAPS_OFF_ACTIVE_GEN_HI);
	out->stix_version = get_u16le(p, ST11_CAPS_OFF_STIX_VERSION);
	return true;
}
