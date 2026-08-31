/*
 * st_scrub.c — see st_scrub.h. PURE. Direct port of src/audio/inertia.ts's
 * shape()/shapeIntegral()/inertiaRateAt()/inertiaDistance()/
 * inertiaZeroCrossing()/makeScrubReleaseSegment() for the scrub-release
 * case (from = signed shuttle rate, to = +1.0 always).
 */

#include "st_scrub.h"

#include <math.h>

static float shape(float u, float k)
{
	if (u <= 0.0f) {
		return 1.0f;
	}
	if (u >= 1.0f) {
		return 0.0f;
	}
	return (expf(-k * u) - expf(-k)) / (1.0f - expf(-k));
}

static float shape_integral(float x, float k)
{
	float xc;

	if (x <= 0.0f) {
		return 0.0f;
	}
	xc = (x < 1.0f) ? x : 1.0f;
	return (((1.0f - expf(-k * xc)) / k) - xc * expf(-k)) / (1.0f - expf(-k));
}

st_scrub_release_t st_scrub_make_release(float from_rate_signed)
{
	st_scrub_release_t seg;
	float to = 1.0f; /* musical rate is always +1.0x for a scrub release */
	float span = fabsf(to - from_rate_signed);
	float reference = fmaxf(fmaxf(fabsf(from_rate_signed), fabsf(to)), 1e-6f);
	float ratio = span / reference;
	float scaled = ST_SCRUB_RELEASE_NOMINAL_S * fminf(2.0f, ratio);

	seg.from = from_rate_signed;
	seg.to = to;
	seg.duration_s = fmaxf(ST_SCRUB_RELEASE_MIN_S, fminf(ST_SCRUB_RELEASE_MAX_S, scaled));
	return seg;
}

float st_scrub_rate_at(const st_scrub_release_t *seg, float dt)
{
	if (dt <= 0.0f) {
		return seg->from;
	}
	if (dt >= seg->duration_s) {
		return seg->to;
	}
	return seg->to + (seg->from - seg->to) * shape(dt / seg->duration_s, ST_SCRUB_INERTIA_K);
}

float st_scrub_distance(const st_scrub_release_t *seg, float dt)
{
	float d = seg->duration_s;
	float k = ST_SCRUB_INERTIA_K;

	if (dt <= 0.0f) {
		return 0.0f;
	}
	if (dt >= d) {
		float full = d * (seg->to * 1.0f + (seg->from - seg->to) * shape_integral(1.0f, k));

		return full + (dt - d) * seg->to;
	}
	{
		float x = dt / d;

		return d * (seg->to * x + (seg->from - seg->to) * shape_integral(x, k));
	}
}

float st_scrub_zero_crossing(const st_scrub_release_t *seg)
{
	float k = ST_SCRUB_INERTIA_K;
	float s, e, u;
	bool same_sign;

	if (seg->from == 0.0f) {
		return 0.0f;
	}
	same_sign = (seg->from > 0.0f) == (seg->to > 0.0f);
	if (same_sign) {
		return -1.0f;
	}
	s = -seg->to / (seg->from - seg->to);
	if (!(s > 0.0f && s < 1.0f)) {
		return -1.0f;
	}
	e = expf(-k);
	u = -logf(s * (1.0f - e) + e) / k;
	if (!(u > 0.0f && u < 1.0f)) {
		return -1.0f;
	}
	return u * seg->duration_s;
}
