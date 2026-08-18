/*
 * st_checksum32.c — see st_checksum32.h. PURE.
 */

#include "st_checksum32.h"

uint32_t st_checksum32_update(uint32_t h, const uint8_t *data, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		h ^= data[i];
		h = h * 0x01000193u; /* FNV prime, mod 2^32 via natural uint32_t wrap */
	}
	return h;
}

uint32_t st_checksum32_compute(const uint8_t *data, size_t len)
{
	return st_checksum32_update(ST_CHECKSUM32_INIT, data, len);
}
