/*
 * st_fx.c — see st_fx.h.
 */

#include "st_fx.h"

#include <string.h>

/* ---------------------------------------------------------------------- *
 * FILTER — lowpass 1800 Hz, Q 0.9, at the committed default macro 0.50.
 *
 * RBJ lowpass, normalised by a0, in Q14. The accumulator is int64 because
 * a1 * y reaches ~9.2e11 in Q23 * Q14, which int32 cannot hold; on Cortex-M4
 * that is a single SMLAL per term, so the width costs nothing.
 *
 *   w0    = 2*pi*1800/48000 = 0.235619
 *   alpha = sin(w0)/(2*0.9)  = 0.129694
 *   b0=b2 = 0.012229  b1 = 0.024458  a1 = -1.721500  a2 = 0.770396
 * ---------------------------------------------------------------------- */
#define FQ 14
#define FILT_B0   200
#define FILT_B1   401
#define FILT_B2   200
#define FILT_A1 (-28205)
#define FILT_A2   12622

static int32_t biquad(st_fx_biquad_t *s, int ch, int32_t x,
		       int32_t b0, int32_t b1, int32_t b2, int32_t a1, int32_t a2)
{
	int64_t acc = (int64_t)b0 * x
		    + (int64_t)b1 * s->x1[ch]
		    + (int64_t)b2 * s->x2[ch]
		    - (int64_t)a1 * s->y1[ch]
		    - (int64_t)a2 * s->y2[ch];
	int32_t y = (int32_t)(acc >> FQ);

	s->x2[ch] = s->x1[ch];
	s->x1[ch] = x;
	s->y2[ch] = s->y1[ch];
	s->y1[ch] = y;
	return y;
}

/* ---------------------------------------------------------------------- *
 * DISTORTION — tanh(15x)/tanh(15) at the committed default macro 0.35,
 * then a taming lowpass at 8 kHz and a 0.8425 trim.
 *
 * The reference builds a 1024-point WaveShaper curve and lets the browser
 * interpolate. Here the curve is a 65-entry quarter table over x in [0,1]
 * with linear interpolation, exploiting tanh's oddness for the negative half.
 * It lives in FLASH, not RAM: the RAM budget counts no table.
 *
 * tanh(15) = 0.99999999999981, so the /tanh(15) normalisation is unity to
 * well beyond int32 precision and is folded away.
 * ---------------------------------------------------------------------- */
#define DIRT_TBL_N 65
/* tanh(15 * i/64) / tanh(15), Q15, i = 0..64. GENERATED, not hand-written:
 * see tests/test_fx_dsp.c, which regenerates it from the same expression and
 * fails if a single entry drifts. */
static const uint16_t dirt_tbl[DIRT_TBL_N] = {
	    0,  7542, 14325, 19868, 24053, 27029, 29054, 30393,
	31261, 31817, 32169, 32391, 32531, 32619, 32675, 32709,
	32731, 32744, 32753, 32758, 32761, 32764, 32765, 32766,
	32766, 32766, 32767, 32767, 32767, 32767, 32767, 32767,
	32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
	32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
	32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
	32767, 32767, 32767, 32767, 32767, 32767, 32767, 32767,
	32767,
};

/* 8 kHz lowpass, Q 0.707, Q14 — the "tame" stage after the shaper. */
#define TAME_B0   2540
#define TAME_B1   5081
#define TAME_B2   2540
#define TAME_A1 (-10161)
#define TAME_A2   3939

/* 0.8425 in Q15. */
#define DIRT_TRIM_Q15 27607

int32_t st_fx_shape_dirt(int32_t x_q23)
{
	int32_t sign = 1;
	uint32_t mag, idx, frac;
	int32_t lo, hi, y;

	if (x_q23 < 0) {
		sign = -1;
		x_q23 = -x_q23;
	}
	mag = (uint32_t)x_q23;
	if (mag >= ST_FX_FULLSCALE) {
		mag = ST_FX_FULLSCALE - 1u;
	}
	/* 64 intervals across [0, 1). */
	idx = (mag * 64u) >> ST_FX_SHIFT;
	frac = ((mag * 64u) & (ST_FX_FULLSCALE - 1u));   /* Q23 within the step */
	lo = (int32_t)dirt_tbl[idx];
	hi = (int32_t)dirt_tbl[idx + 1u];
	/* Q15 curve value, linearly interpolated. */
	y = lo + (int32_t)(((int64_t)(hi - lo) * (int64_t)frac) >> ST_FX_SHIFT);
	/* Q15 -> Q23. */
	y = (int32_t)(((int64_t)y << ST_FX_SHIFT) >> 15);
	return sign * y;
}

/* ---------------------------------------------------------------------- *
 * helpers
 * ---------------------------------------------------------------------- */
static int32_t clamp_q23(int32_t v)
{
	if (v > ST_FX_FULLSCALE - 1) return ST_FX_FULLSCALE - 1;
	if (v < -ST_FX_FULLSCALE) return -ST_FX_FULLSCALE;
	return v;
}

/* Correlated complementary mix: dry = 1 - wet. NOT equal power -- both legs
 * carry the same source, which is exactly the case where equal-power is
 * wrong (banks.ts:867-871). */
static int32_t mix_wet(int32_t dry, int32_t wet, uint16_t w)
{
	int32_t d = ST_FX_WET_UNITY - (int32_t)w;

	return (int32_t)(((int64_t)dry * d + (int64_t)wet * (int32_t)w)
			  >> ST_FX_WET_SHIFT);
}

void st_fx_reset(st_fx_t *fx)
{
	memset(fx, 0, sizeof(*fx));
}

void st_fx_prepare(st_fx_t *fx, uint32_t frames_per_beat,
		    uint32_t downbeat_frame, uint8_t active_mask)
{
	fx->frames_per_beat = frames_per_beat;
	fx->downbeat_frame = downbeat_frame;

	if (frames_per_beat != 0u) {
		uint32_t d = (frames_per_beat * ST_FX_ECHO_DIV_NUM) / ST_FX_ECHO_DIV_DEN;

		if (d < 1u) d = 1u;
		if (d > ST_FX_ECHO_MAX_FRAMES) d = ST_FX_ECHO_MAX_FRAMES;
		fx->echo_len = d;
		fx->gate_cycle = frames_per_beat / ST_FX_GATE_CYCLES_PER_BEAT;
	} else {
		/* No trustworthy tempo: the two tempo-locked effects fail
		 * closed rather than inventing a grid. */
		fx->echo_len = 0u;
		fx->gate_cycle = 0u;
	}

	/* A newly released echo keeps circulating for a bounded tail. */
	if ((fx->active & ST_FX_BIT(ST_FX_ECHO)) != 0u &&
	    (active_mask & ST_FX_BIT(ST_FX_ECHO)) == 0u) {
		fx->echo_tail = ST_FX_ECHO_TAIL_FRAMES;
	}
	fx->active = active_mask;
}

bool st_fx_running(const st_fx_t *fx)
{
	uint8_t e;

	if (fx->active != 0u || fx->echo_tail != 0u) {
		return true;
	}
	for (e = 0; e < ST_FX_COUNT; e++) {
		if (fx->wet[e] != 0u) {
			return true;
		}
	}
	return false;
}

/* Advance one effect's engage ramp by one frame and return the gain to use. */
static uint16_t wet_step(st_fx_t *fx, uint8_t e)
{
	const uint16_t target = ((fx->active & ST_FX_BIT(e)) != 0u)
				? ST_FX_WET_UNITY : 0u;
	const uint16_t step = ST_FX_WET_UNITY / ST_FX_ENGAGE_FRAMES;   /* 56 */
	uint16_t w = fx->wet[e];

	if (w < target) {
		w = (uint16_t)((w + step > target) ? target : w + step);
	} else if (w > target) {
		w = (w < step) ? 0u : (uint16_t)(w - step);
	}
	fx->wet[e] = w;
	return w;
}

void st_fx_process(st_fx_t *fx, int32_t *l, int32_t *r, uint32_t song_frame)
{
	int32_t dl = *l, dr = *r;
	uint16_t w;
	int i;

	for (i = 0; i < (int)ST_FX_COUNT; i++) {
		const uint8_t e = st_fx_signal_order[i];

		w = wet_step(fx, e);

		switch (e) {
		case ST_FX_FILTER: {
			int32_t fl, fr;

			if (w == 0u) break;
			fl = biquad(&fx->filter, 0, dl,
				     FILT_B0, FILT_B1, FILT_B2, FILT_A1, FILT_A2);
			fr = biquad(&fx->filter, 1, dr,
				     FILT_B0, FILT_B1, FILT_B2, FILT_A1, FILT_A2);
			dl = mix_wet(dl, clamp_q23(fl), w);
			dr = mix_wet(dr, clamp_q23(fr), w);
			break;
		}

		case ST_FX_DIRT: {
			int32_t sl, sr;

			if (w == 0u) break;
			sl = st_fx_shape_dirt(dl);
			sr = st_fx_shape_dirt(dr);
			sl = biquad(&fx->dirt_tame, 0, sl,
				     TAME_B0, TAME_B1, TAME_B2, TAME_A1, TAME_A2);
			sr = biquad(&fx->dirt_tame, 1, sr,
				     TAME_B0, TAME_B1, TAME_B2, TAME_A1, TAME_A2);
			/* Loudness compensation: hard drive must not simply be
			 * louder (banks.ts:228). */
			sl = (int32_t)(((int64_t)sl * DIRT_TRIM_Q15) >> 15);
			sr = (int32_t)(((int64_t)sr * DIRT_TRIM_Q15) >> 15);
			dl = mix_wet(dl, clamp_q23(sl), w);
			dr = mix_wet(dr, clamp_q23(sr), w);
			break;
		}

		case ST_FX_GATE: {
			uint32_t pos, half, g;
			int32_t gl, gr;

			if (w == 0u || fx->gate_cycle == 0u) break;
			/* PHASE FROM THE PLAYBACK FRAME, never a free-running
			 * counter: a loop wrap moves song_frame and the gate
			 * follows it exactly, with nothing to resync. */
			if (song_frame < fx->downbeat_frame) break;
			pos = (song_frame - fx->downbeat_frame) % fx->gate_cycle;
			half = fx->gate_cycle / 2u;

			if (pos < half) {
				/* open, with a short ramp in */
				g = (pos < ST_FX_GATE_EDGE_FRAMES)
				    ? (pos * ST_FX_WET_UNITY) / ST_FX_GATE_EDGE_FRAMES
				    : ST_FX_WET_UNITY;
			} else {
				uint32_t into = pos - half;

				g = (into < ST_FX_GATE_EDGE_FRAMES)
				    ? ST_FX_WET_UNITY -
				      (into * ST_FX_WET_UNITY) / ST_FX_GATE_EDGE_FRAMES
				    : 0u;
			}
			gl = (int32_t)(((int64_t)dl * (int32_t)g) >> ST_FX_WET_SHIFT);
			gr = (int32_t)(((int64_t)dr * (int32_t)g) >> ST_FX_WET_SHIFT);
			dl = mix_wet(dl, gl, w);
			dr = mix_wet(dr, gr, w);
			break;
		}

		case ST_FX_ECHO: {
			int32_t in_mono, rd, fb;
			uint32_t ri;

			if (fx->echo_len == 0u) break;
			if (w == 0u && fx->echo_tail == 0u) break;
			if (w == 0u) fx->echo_tail--;

			/* read the tap */
			ri = (fx->echo_w + ST_FX_ECHO_MAX_FRAMES - fx->echo_len)
			     % ST_FX_ECHO_MAX_FRAMES;
			rd = (int32_t)fx->echo_line[ri] << 8;   /* Q15 -> Q23 */

			/* one-pole damping lowpass at 7.8 kHz (banks.ts:435,
			 * pitched=false). alpha = 1 - exp(-2*pi*7800/48000)
			 * = 0.63977 -> 20964 in Q15. */
			fx->echo_damp += (int32_t)((((int64_t)(rd - fx->echo_damp)) * 20964) >> 15);
			rd = fx->echo_damp;

			/* write input + damped feedback */
			in_mono = (dl >> 1) + (dr >> 1);
			fb = (int32_t)(((int64_t)rd * ST_FX_ECHO_FEEDBACK_Q15) >> 15);
			fx->echo_line[fx->echo_w] =
				(int16_t)(clamp_q23(in_mono + fb) >> 8);
			fx->echo_w = (fx->echo_w + 1u) % ST_FX_ECHO_MAX_FRAMES;

			/* THE WET LEG IS THE REPEATS ONLY. banks.ts:436-440
			 * connects input->delay->damp->out with NO direct
			 * input->out edge, unlike every other algorithm in that
			 * file. Engaging the echo therefore replaces the source
			 * with its repeats -- a delay throw. That is the
			 * committed reference and it is implemented faithfully
			 * rather than quietly "corrected". */
			dl = mix_wet(dl, clamp_q23(rd), w);
			dr = mix_wet(dr, clamp_q23(rd), w);
			break;
		}

		default:
			break;
		}
	}

	*l = clamp_q23(dl);
	*r = clamp_q23(dr);
}
