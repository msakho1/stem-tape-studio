/*
 * st_crc32.c — see st_crc32.h. PURE.
 */

#include "st_crc32.h"

#include "st_transfer_protocol.h"

uint32_t st_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
	size_t i;
	int b;

	for (i = 0; i < len; i++) {
		crc ^= data[i];
		for (b = 0; b < 8; b++) {
			uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));

			crc = (crc >> 1) ^ (ST_CRC32_POLY_REFLECTED & mask);
		}
	}
	return crc;
}

uint32_t st_crc32_compute(const uint8_t *data, size_t len)
{
	return st_crc32_update(ST_CRC32_INIT, data, len) ^ 0xFFFFFFFFu;
}
