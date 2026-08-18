/*
 * st_sector_v11.c — see st_sector_v11.h. PURE.
 */

#include "st_sector_v11.h"

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

/* Conventional signed 24-bit little-endian: byte0=LSB, byte1=mid,
 * byte2=MSB -- matching src/sp1/song.ts's packStereo24()/readInt24LE(). */
static void put_i24le(uint8_t *out, uint32_t off, int32_t v)
{
	uint32_t u = (uint32_t)v & 0xFFFFFFu; /* keep low 24 bits; caller guarantees in-range */

	out[off + 0] = (uint8_t)(u & 0xffu);
	out[off + 1] = (uint8_t)((u >> 8) & 0xffu);
	out[off + 2] = (uint8_t)((u >> 16) & 0xffu);
}

static int32_t get_i24le(const uint8_t *in, uint32_t off)
{
	uint32_t v = (uint32_t)in[off + 0] | ((uint32_t)in[off + 1] << 8) |
		     ((uint32_t)in[off + 2] << 16);

	if (v & 0x800000u) {
		v |= 0xFF000000u; /* sign-extend into the top byte */
	}
	return (int32_t)v;
}

void st11_sector_encode(uint32_t sector_index, uint32_t first_frame, uint32_t frame_count,
			 uint32_t bpm_q8, uint32_t downbeat_frame,
			 const st11_audio_frame_t *frames, uint8_t out[ST11_SECTOR_BYTES])
{
	uint32_t f;
	uint32_t s;

	memset(out, 0, ST11_SECTOR_BYTES);

	put_u32le(out, ST11_SECTOR_OFF_MAGIC, ST11_SECTOR_MAGIC);
	put_u32le(out, ST11_SECTOR_OFF_SECTOR_INDEX, sector_index);
	put_u32le(out, ST11_SECTOR_OFF_FIRST_FRAME, first_frame);
	put_u32le(out, ST11_SECTOR_OFF_FRAME_COUNT, frame_count);
	put_u32le(out, ST11_SECTOR_OFF_BPM_Q8, bpm_q8);
	put_u32le(out, ST11_SECTOR_OFF_DOWNBEAT, downbeat_frame);
	/* ledReserved + reserved stay zero: firmware-owned bytes, never
	 * invented here -- matching the companion's own encodeSector(). */

	if (frame_count > ST11_FRAMES_PER_SECTOR) {
		frame_count = ST11_FRAMES_PER_SECTOR; /* fail closed: never overrun the buffer */
	}

	for (f = 0; f < frame_count; f++) {
		uint32_t frame_off = ST11_SECTOR_HEADER_BYTES + f * ST11_BYTES_PER_FRAME;

		for (s = 0; s < ST11_STEM_COUNT; s++) {
			uint32_t stem_off = frame_off + s * ST11_STEM_FRAME_BYTES;

			put_i24le(out, stem_off, frames[f].stem_l[s]);
			put_i24le(out, stem_off + ST11_BYTES_PER_SAMPLE, frames[f].stem_r[s]);
		}
	}
}

bool st11_sector_read_header(const uint8_t in[ST11_SECTOR_BYTES], st11_sector_header_t *header_out)
{
	uint32_t magic = get_u32le(in, ST11_SECTOR_OFF_MAGIC);

	if (magic != ST11_SECTOR_MAGIC) {
		return false;
	}
	header_out->sector_index = get_u32le(in, ST11_SECTOR_OFF_SECTOR_INDEX);
	header_out->first_frame = get_u32le(in, ST11_SECTOR_OFF_FIRST_FRAME);
	header_out->frame_count = get_u32le(in, ST11_SECTOR_OFF_FRAME_COUNT);
	header_out->bpm_q8 = get_u32le(in, ST11_SECTOR_OFF_BPM_Q8);
	header_out->downbeat_frame = get_u32le(in, ST11_SECTOR_OFF_DOWNBEAT);
	return true;
}

void st11_sector_decode_frame(const uint8_t in[ST11_SECTOR_BYTES], uint32_t frame_index,
			       st11_audio_frame_t *frame_out)
{
	uint32_t frame_off = ST11_SECTOR_HEADER_BYTES + frame_index * ST11_BYTES_PER_FRAME;
	uint32_t s;

	for (s = 0; s < ST11_STEM_COUNT; s++) {
		uint32_t stem_off = frame_off + s * ST11_STEM_FRAME_BYTES;

		frame_out->stem_l[s] = get_i24le(in, stem_off);
		frame_out->stem_r[s] = get_i24le(in, stem_off + ST11_BYTES_PER_SAMPLE);
	}
}
