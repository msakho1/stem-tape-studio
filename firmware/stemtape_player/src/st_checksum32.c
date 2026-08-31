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

uint32_t st_checksum32_unfold_zeros(uint32_t h, size_t count)
{
	size_t i;

	/* h' = (h ^ 0) * PRIME  ==  h * PRIME, so the step inverts to a
	 * single multiply by PRIME's modular inverse. The multiplicative
	 * group mod 2^32 contains every odd number, and 0x01000193 is odd,
	 * so the inverse exists and is exact -- this is not an
	 * approximation of the un-fold, it IS the un-fold. */
	for (i = 0; i < count; i++) {
		h = h * ST_CHECKSUM32_PRIME_INV;
	}
	return h;
}
