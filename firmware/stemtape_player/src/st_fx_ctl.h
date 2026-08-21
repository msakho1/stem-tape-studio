/*
 * st_fx_ctl.h — the FX control overlay: chord entry, scope, target, momentary
 * and latch. Pure: no Zephyr, no allocation, no clock of its own, no audio.
 *
 * main.c calls st_fx_ctl_service() once per control pass with the raw button
 * states and a millisecond stamp, and gets back a complete picture: whether
 * the overlay is open, which scope, which stem the one rack sits on, which
 * effects are sounding, and which of the raw button events the overlay has
 * CONSUMED so no normal-mode handler may also process them.
 *
 * ======================================================================
 * ONE RACK. NOT FIVE.
 * ======================================================================
 * There is exactly one DSP rack. STEM scope inserts it on one selected stem;
 * GLOBAL scope inserts it over the complete audible mix. Walking the target
 * MOVES that rack -- the stem it leaves returns to dry, and no second rack is
 * created. Latch state belongs to the rack and follows it.
 *
 * ======================================================================
 * THE FOUR FIXED EFFECTS
 * ======================================================================
 * Physical button order is NOT the DSP order, and both matter:
 *
 *   button   effect          DSP position
 *   T1       Filter          1st
 *   T2       Delay/Echo      4th   <-- last, so it repeats the finished sound
 *   T3       Distortion      2nd
 *   T4       Gate/Stutter    3rd
 *
 * Every mask in this header is in BUTTON order. st_fx_signal_order[] maps a
 * DSP slot to the button-order index, and it is the only place the two
 * orderings are allowed to meet.
 */

#ifndef ST_FX_CTL_H_
#define ST_FX_CTL_H_

#include <stdbool.h>
#include <stdint.h>

/* ---- effects, in PHYSICAL BUTTON order ------------------------------- */
#define ST_FX_FILTER 0u   /* T1 */
#define ST_FX_ECHO   1u   /* T2 */
#define ST_FX_DIRT   2u   /* T3 */
#define ST_FX_GATE   3u   /* T4 */
#define ST_FX_COUNT  4u

#define ST_FX_BIT(e) ((uint8_t)(1u << (e)))

/*
 * DSP ORDER: Filter -> Distortion -> Gate/Stutter -> Delay/Echo.
 * Entry i is the button-order index of the i-th effect in the signal chain.
 * Filter shapes the source, Distortion works on the filtered source, the Gate
 * chops that result, and the Echo repeats the completed processed sound.
 */
extern const uint8_t st_fx_signal_order[ST_FX_COUNT];

/* ---- scope ------------------------------------------------------------ */
typedef enum {
	ST_FX_SCOPE_STEM = 0,
	ST_FX_SCOPE_GLOBAL,
} st_fx_scope_t;

#define ST_FX_STEM_COUNT 4u   /* Vocal, Drums, Bass, Instrument */

/* ---- timing, from the resolved contract ------------------------------- */
/* Both Volume buttons must go down within this of each other. Resolved to 120
 * (docs/firmware-contract-v1.json timing.fxOverlaySecondPressMs) rather than
 * the running arbiter's 400: this window is dead latency on EVERY ordinary
 * volume press, because a single press must be withheld until the chord can be
 * ruled out. */
#define ST_FX_CHORD_ARRIVAL_MS 120u
/* Both released inside this (from the later down) toggles the overlay. */
#define ST_FX_CHORD_RELEASE_MS 600u
/* At or past this the chord is the stock pairing gesture, never an FX toggle. */
#define ST_FX_CHORD_PAIRING_MS 2000u

/* ---- input ------------------------------------------------------------ */
typedef struct {
	bool     vol_minus_down;
	bool     vol_plus_down;
	bool     function_down;
	uint8_t  track_down;    /* bit k set = Track (k+1) physically down */
	uint32_t now_ms;
} st_fx_in_t;

/* ---- output ----------------------------------------------------------- */
typedef struct {
	bool          fx_open;
	st_fx_scope_t scope;
	uint8_t       target_stem;      /* 0..3; meaningless in GLOBAL scope */

	uint8_t       momentary_mask;   /* button order */
	uint8_t       latch_mask;       /* button order */
	uint8_t       active_mask;      /* momentary | latch */

	/* One-shot edges, true for exactly one service pass. */
	bool          opened;
	bool          closed;
	bool          target_changed;

	/* CONSUMPTION. When true, the named input has been claimed by the
	 * overlay and no normal-mode handler may act on it this pass. */
	bool          vol_minus_consumed;
	bool          vol_plus_consumed;
	bool          function_consumed;
	uint8_t       track_consumed;   /* bit k = Track (k+1) claimed */

	/* Ordinary volume actions the overlay has decided NOT to claim. Each is
	 * true for exactly one pass, on the frame the chord window ruled the
	 * chord out. main.c dispatches the normal volume step on these. */
	bool          vol_minus_fire;
	bool          vol_plus_fire;

	/* Diagnostics only -- never an LED, never an action. */
	bool          pairing;
	bool          ambiguous;
} st_fx_out_t;

/* ---- state ------------------------------------------------------------ */
typedef enum {
	ST_FX_CHORD_IDLE = 0,
	ST_FX_CHORD_ONE_DOWN,     /* one Volume down, arrival window running */
	ST_FX_CHORD_SINGLE,       /* window expired with one down: it is a single */
	ST_FX_CHORD_ARMED,        /* both down inside the window */
	ST_FX_CHORD_WAIT_RELEASE, /* resolved; both must lift before another */
} st_fx_chord_state_t;

typedef struct {
	/* overlay */
	bool          open;
	st_fx_scope_t scope;
	uint8_t       target_stem;
	uint8_t       momentary;
	uint8_t       latch;

	/* chord */
	st_fx_chord_state_t chord;
	uint32_t      first_down_ms;
	uint32_t      second_down_ms;
	bool          first_was_minus;
	bool          chord_fn;        /* FUNCTION at CHORD BEGIN, latched */

	/* edges */
	bool          prev_minus;
	bool          prev_plus;
	bool          prev_function;
	uint8_t       prev_track;
} st_fx_ctl_t;

void st_fx_ctl_reset(st_fx_ctl_t *s);

/*
 * One control pass. `out` is fully written every call -- no field carries a
 * value from a previous pass.
 */
void st_fx_ctl_service(st_fx_ctl_t *s, const st_fx_in_t *in, st_fx_out_t *out);

#endif /* ST_FX_CTL_H_ */
