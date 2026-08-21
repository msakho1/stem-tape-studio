/*
 * st_fx.h — THE one FX rack. Four fixed effects, one instance, no allocation.
 *
 * PURE: no Zephyr, no malloc, no clock. Host-testable in full.
 *
 * ======================================================================
 * ONE RACK, TWO SCOPES
 * ======================================================================
 * There is exactly one instance of this struct in the firmware. In STEM scope
 * it processes one selected stem before the fader and the mix; in GLOBAL scope
 * it processes the finished audible mix. Moving the target moves this rack --
 * the stem it leaves returns to dry immediately, and no second rack exists to
 * allocate.
 *
 * ======================================================================
 * SIGNAL ORDER IS NOT BUTTON ORDER
 * ======================================================================
 *      source -> Filter -> Distortion -> Gate/Stutter -> Delay/Echo -> out
 *      T1                  T3            T4              T2
 *
 * The echo is LAST so it repeats the completed processed sound, exactly as the
 * committed reference chains its stages.
 *
 * ======================================================================
 * SAMPLE DOMAIN
 * ======================================================================
 * Everything here is Q23: sign-extended 24-bit, |x| <= 8388607, which is
 * precisely what st11_sector_decode_frame() produces. GLOBAL scope shifts the
 * mixed int16 up by 8 on the way in and saturates back down on the way out, so
 * there is ONE processing path rather than a 16-bit and a 24-bit variant.
 *
 * The echo's delay line is the single exception: it stores Q15 int16 (the
 * sample >> 8). That halves the only large allocation in the whole feature and
 * costs 8 bits on the repeats, which is the trade the RAM budget was built on.
 *
 * ======================================================================
 * FIXED SETTINGS — the committed reference defaults, evaluated
 * ======================================================================
 * All four come from src/machine/fx12.ts's `defaultMacro` put through
 * src/audio/fx/banks.ts. Nothing here was chosen:
 *
 *   Filter      macro 0.50 -> lowpass 1800 Hz, Q 0.9         banks.ts:104
 *   Distortion  macro 0.35 -> tanh(15x)/tanh(15), trim .8425 banks.ts:202,228
 *   Gate        macro 0.50 -> 4 cycles/beat (1/16), 50% duty banks.ts:391-397
 *   Delay/Echo  macro 0.50 -> 0.375 beat, feedback 0.43      banks.ts:460-463
 *
 * There is no macro, no variation and no algorithm selection. Those gestures
 * were removed by the product decision, so no storage exists for them.
 */

#ifndef ST_FX_H_
#define ST_FX_H_

#include <stdbool.h>
#include <stdint.h>

#include "st_fx_ctl.h"   /* ST_FX_FILTER/ECHO/DIRT/GATE, ST_FX_COUNT */

/* ---- sample domain ---------------------------------------------------- */
#define ST_FX_SHIFT     23
#define ST_FX_FULLSCALE (1 << ST_FX_SHIFT)   /* 8388608 */

/* ---- engagement ------------------------------------------------------- */
/* FX_ENGAGE_S 0.012 at 48 kHz. Sample-level and exact -- not a block count. */
#define ST_FX_ENGAGE_FRAMES 576u
#define ST_FX_WET_SHIFT 15
#define ST_FX_WET_UNITY (1 << ST_FX_WET_SHIFT)   /* 32768 */

/* ---- echo ------------------------------------------------------------- */
/* 0.375 beat, as 3/8, in integer arithmetic. */
#define ST_FX_ECHO_DIV_NUM 3u
#define ST_FX_ECHO_DIV_DEN 8u
/* The slowest admitted tempo. The firmware has no BPM clamp of its own
 * (st_beat_phase.c accepts any nonzero bpm_q8), so THIS is the clamp, and it
 * is what sizes the delay line: 0.375 * 48000 * 60 / 60 = 18000 frames. */
#define ST_FX_ECHO_MIN_BPM 60u
#define ST_FX_ECHO_MAX_FRAMES 18000u
/* Feedback at the reference default, Q15. 0.43 * 32768 = 14090. */
#define ST_FX_ECHO_FEEDBACK_Q15 14090
/* Contract ceiling, enforced regardless of anything else. 0.72 * 32768. */
#define ST_FX_ECHO_FEEDBACK_MAX_Q15 23593
/* How long the delay keeps circulating behind a silent output after release,
 * so re-engaging resumes a natural tail instead of a frozen one. At feedback
 * 0.43 the loop is 73 dB down after ten repeats; this is comfortably past
 * that at any admitted tempo, and costs ~0.7% of one core while it runs. */
#define ST_FX_ECHO_TAIL_FRAMES (48000u * 3u)

/* ---- gate ------------------------------------------------------------- */
/* 4 cycles per beat = 1/16 notes at the reference default macro. */
#define ST_FX_GATE_CYCLES_PER_BEAT 4u
/* Edge ramp. The reference drives a Web Audio square oscillator, which is
 * band-limited; a raw 0<->1 step at 48 kHz is not, and would click on every
 * sixteenth. 32 frames is 0.67 ms -- far too fast to soften the chop, far too
 * slow to produce a step edge. This is a deliberate, documented deviation. */
#define ST_FX_GATE_EDGE_FRAMES 32u

/* ---- state ------------------------------------------------------------ */
typedef struct {
	int32_t x1[2], x2[2], y1[2], y2[2];   /* per channel */
} st_fx_biquad_t;

typedef struct {
	/* engage ramps, one per effect, in BUTTON order */
	uint16_t wet[ST_FX_COUNT];       /* 0..ST_FX_WET_UNITY */
	uint8_t  active;                 /* last active mask seen */

	st_fx_biquad_t filter;
	st_fx_biquad_t dirt_tame;

	/* echo */
	int16_t  echo_line[ST_FX_ECHO_MAX_FRAMES];   /* Q15 mono */
	uint32_t echo_w;                 /* write index */
	uint32_t echo_len;               /* live delay length in frames */
	int32_t  echo_damp;              /* one-pole LP state, Q23 */
	uint32_t echo_tail;              /* frames left to keep circulating */

	/* gate */
	uint32_t gate_cycle;             /* frames per gate cycle, 0 = no tempo */

	/* tempo, refreshed once per block */
	uint32_t frames_per_beat;
	uint32_t downbeat_frame;
} st_fx_t;

void st_fx_reset(st_fx_t *fx);

/*
 * Once per audio block. Recomputes the tempo-derived quantities and latches
 * the active mask. Cheap and idempotent; safe to call every block.
 */
void st_fx_prepare(st_fx_t *fx, uint32_t frames_per_beat,
		    uint32_t downbeat_frame, uint8_t active_mask);

/*
 * True when the rack has anything to do. When this is false the caller must
 * skip st_fx_process() entirely -- that is what keeps FX off the CPU budget
 * when the overlay has never been opened.
 */
bool st_fx_running(const st_fx_t *fx);

/*
 * ONE FRAME, in place. `*l`/`*r` are Q23 in and Q23 out. `song_frame` is the
 * authoritative playback position -- the gate's phase is derived from it and
 * from the downbeat, so there is no second clock and a loop wrap cannot
 * desync it.
 */
void st_fx_process(st_fx_t *fx, int32_t *l, int32_t *r, uint32_t song_frame);

/* Exposed for the reference-vector tests. */
int32_t st_fx_shape_dirt(int32_t x_q23);

#endif /* ST_FX_H_ */
