/*
 * st_sector_codec.c — see st_sector_codec.h. PURE.
 */

#include "st_sector_codec.h"

#include <string.h>

/* 24-bit sign extension: `v` holds the low 24 bits of a two's-complement
 * sample in a uint32_t; returns the sign-extended int32_t value. */
static int32_t sign_extend_24(uint32_t v)
{
	if (v & 0x00800000u) {
		return (int32_t)(v | 0xFF000000u);
	}
	return (int32_t)v;
}

static uint32_t clamp_u24(int32_t v)
{
	if (v > 0x7FFFFF) {
		v = 0x7FFFFF;
	}
	if (v < -0x800000) {
		v = -0x800000;
	}
	return ((uint32_t)v) & 0x00FFFFFFu;
}

/* Encodes one stem's L/R pair into the documented 6-byte layout:
 *   byte0 = L mid, byte1 = L MSB, byte2 = R MSB,
 *   byte3 = L LSB, byte4 = R LSB, byte5 = R mid
 * [wiki timknapen/SP-1-dev "Audio format"] */
static void encode_stem_frame(int32_t l, int32_t r, uint8_t out[ST_STEM_FRAME_BYTES])
{
	uint32_t lu = clamp_u24(l);
	uint32_t ru = clamp_u24(r);
	uint8_t l_msb = (uint8_t)((lu >> 16) & 0xFFu);
	uint8_t l_mid = (uint8_t)((lu >> 8) & 0xFFu);
	uint8_t l_lsb = (uint8_t)(lu & 0xFFu);
	uint8_t r_msb = (uint8_t)((ru >> 16) & 0xFFu);
	uint8_t r_mid = (uint8_t)((ru >> 8) & 0xFFu);
	uint8_t r_lsb = (uint8_t)(ru & 0xFFu);

	out[0] = l_mid;
	out[1] = l_msb;
	out[2] = r_msb;
	out[3] = l_lsb;
	out[4] = r_lsb;
	out[5] = r_mid;
}

static void decode_stem_frame(const uint8_t in[ST_STEM_FRAME_BYTES], int32_t *l, int32_t *r)
{
	uint32_t lu = ((uint32_t)in[1] << 16) | ((uint32_t)in[0] << 8) | (uint32_t)in[3];
	uint32_t ru = ((uint32_t)in[2] << 16) | ((uint32_t)in[5] << 8) | (uint32_t)in[4];

	*l = sign_extend_24(lu);
	*r = sign_extend_24(ru);
}

void st_sector_encode(const st_audio_frame_t frames[ST_SECTOR_FRAME_CAPACITY],
		       const st_sector_reserved_t *reserved,
		       uint8_t out[ST_SECTOR_BYTES])
{
	uint32_t sb;
	uint32_t f;
	uint32_t s;

	for (sb = 0; sb < ST_SUBBLOCK_COUNT; sb++) {
		uint8_t physical = ST_SUBBLOCK_PHYSICAL_ORDER[sb];
		uint8_t *base = out + (uint32_t)physical * ST_SUBBLOCK_BYTES;
		uint8_t *p = base;

		for (f = 0; f < ST_SUBBLOCK_FRAMES; f++) {
			uint32_t global_frame = sb * ST_SUBBLOCK_FRAMES + f;
			const st_audio_frame_t *fr = &frames[global_frame];

			for (s = 0; s < ST_STEM_COUNT; s++) {
				encode_stem_frame(fr->stem_l[s], fr->stem_r[s], p);
				p += ST_STEM_FRAME_BYTES;
			}
		}
		/* Reserved 8 bytes: sync(2) + tempo(2) + LED(4). */
		p[0] = (uint8_t)(reserved->sync[sb] & 0xFFu);
		p[1] = (uint8_t)((reserved->sync[sb] >> 8) & 0xFFu);
		p[2] = (uint8_t)(reserved->tempo[sb] & 0xFFu);
		p[3] = (uint8_t)((reserved->tempo[sb] >> 8) & 0xFFu);
		p[4] = reserved->led[sb][0];
		p[5] = reserved->led[sb][1];
		p[6] = reserved->led[sb][2];
		p[7] = reserved->led[sb][3];
	}
}

void st_sector_decode(const uint8_t in[ST_SECTOR_BYTES],
		       st_audio_frame_t frames_out[ST_SECTOR_FRAME_CAPACITY],
		       st_sector_reserved_t *reserved_out)
{
	uint32_t sb;
	uint32_t f;
	uint32_t s;

	memset(frames_out, 0, sizeof(*frames_out) * ST_SECTOR_FRAME_CAPACITY);
	if (reserved_out != NULL) {
		memset(reserved_out, 0, sizeof(*reserved_out));
	}

	for (sb = 0; sb < ST_SUBBLOCK_COUNT; sb++) {
		uint8_t physical = ST_SUBBLOCK_PHYSICAL_ORDER[sb];
		const uint8_t *base = in + (uint32_t)physical * ST_SUBBLOCK_BYTES;
		const uint8_t *p = base;

		for (f = 0; f < ST_SUBBLOCK_FRAMES; f++) {
			uint32_t global_frame = sb * ST_SUBBLOCK_FRAMES + f;
			st_audio_frame_t *fr = &frames_out[global_frame];

			for (s = 0; s < ST_STEM_COUNT; s++) {
				decode_stem_frame(p, &fr->stem_l[s], &fr->stem_r[s]);
				p += ST_STEM_FRAME_BYTES;
			}
		}
		if (reserved_out != NULL) {
			reserved_out->sync[sb] = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
			reserved_out->tempo[sb] = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
			reserved_out->led[sb][0] = p[4];
			reserved_out->led[sb][1] = p[5];
			reserved_out->led[sb][2] = p[6];
			reserved_out->led[sb][3] = p[7];
		}
	}
}

void st_sector_decode_frame(const uint8_t in[ST_SECTOR_BYTES], uint32_t frame_index,
			     st_audio_frame_t *frame_out)
{
	uint32_t sb = frame_index / ST_SUBBLOCK_FRAMES;
	uint32_t f = frame_index % ST_SUBBLOCK_FRAMES;
	uint8_t physical = ST_SUBBLOCK_PHYSICAL_ORDER[sb];
	const uint8_t *base = in + (uint32_t)physical * ST_SUBBLOCK_BYTES;
	const uint8_t *p = base + f * ST_FRAME_BYTES;
	uint32_t s;

	for (s = 0; s < ST_STEM_COUNT; s++) {
		decode_stem_frame(p, &frame_out->stem_l[s], &frame_out->stem_r[s]);
		p += ST_STEM_FRAME_BYTES;
	}
}
