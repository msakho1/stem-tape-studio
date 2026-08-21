/*
 * st_led_mvp.c — see st_led_mvp.h. PURE: no Zephyr, no hardware, no globals,
 * no clock.
 */

#include "st_led_mvp.h"

/* ==========================================================================
 * BATTERY GAUGE
 *
 * Ported from firmware/stemtape/src/led_battery.c (which ported it from the
 * pinned Tape Looper's own standby gauge) rather than linked, because that
 * module includes led_duty.h -- the M0 target's PWM2/PWM3 renderer and its
 * led_channel_table[], neither of which exists in this firmware. The
 * arithmetic is kept line-for-line comparable so the two can be diffed.
 * ========================================================================== */

static const int32_t batt_thr[3] = {
	(int32_t)ST_LED_BATT_THR_1,
	(int32_t)ST_LED_BATT_THR_2,
	(int32_t)ST_LED_BATT_THR_3,
};

void st_led_batt_reset(st_led_batt_gauge_t *g)
{
	g->ema = -1;
	g->level = 0u;
	g->ever_valid = false;
	g->last_read_ok = false;
}

void st_led_batt_update(st_led_batt_gauge_t *g, bool valid, int32_t raw_adc)
{
	uint8_t nl;
	int k;

	g->last_read_ok = valid;
	if (!valid) {
		return;   /* sticky: EMA and level unchanged on a failed read */
	}
	g->ever_valid = true;

	/* Integer DIVISION, not a shift: a shift truncates toward negative
	 * infinity on a falling reading where the source truncates toward zero. */
	g->ema = (g->ema < 0) ? raw_adc
			      : g->ema + (raw_adc - g->ema) / (1 << ST_LED_BATT_EMA_SHIFT);

	nl = 1u;
	for (k = 0; k < 3; k++) {
		if (g->ema > batt_thr[k]) {
			nl = (uint8_t)(k + 2);
		}
	}

	if (g->level == 0u) {
		g->level = nl;
	} else if (nl > g->level &&
		   g->ema > batt_thr[g->level - 1u] + (int32_t)ST_LED_BATT_HYSTERESIS_COUNTS) {
		g->level = nl;
	} else if (nl < g->level &&
		   g->ema < batt_thr[g->level - 2u] - (int32_t)ST_LED_BATT_HYSTERESIS_COUNTS) {
		g->level = nl;
	}
}

st_led_batt_state_t st_led_batt_classify(const st_led_batt_gauge_t *g,
					  bool charger_present, bool charging_now)
{
	/* nCHG asserted with nPGOOD deasserted is "charging with no input
	 * power", which the BQ24232 cannot legitimately report. Distinct, and
	 * never "low": a wiring fault must not read as an empty battery. */
	if (charging_now && !charger_present) {
		return ST_LED_BATT_FAULT;
	}
	if (!g->ever_valid) {
		return ST_LED_BATT_UNAVAILABLE;
	}
	if (!g->last_read_ok) {
		return ST_LED_BATT_FAULT;
	}
	if (charger_present) {
		return charging_now ? ST_LED_BATT_CHARGING : ST_LED_BATT_CHARGE_COMPLETE;
	}
	return (g->level <= ST_LED_BATT_LOW_LEVEL) ? ST_LED_BATT_LOW
						   : ST_LED_BATT_CHARGER_ABSENT;
}

/* ========================================================================== */

static void all_dark(st_led_frame_t *out)
{
	uint8_t i;

	for (i = 0; i < ST_LED_COUNT; i++) {
		out->level[i] = 0u;
	}
}

/* Is the battery reading one we are willing to render at all? */
static bool batt_trustworthy(const st_led_inputs_t *in)
{
	return in->batt_state != ST_LED_BATT_UNAVAILABLE &&
	       in->batt_state != ST_LED_BATT_FAULT &&
	       in->batt_level != 0u;
}

/* The four-step gauge on S1..S4, bottom-up. `blink_step` blinks the current
 * (topmost lit) step, which is what the charging display wants; the static
 * preview passes false and shows every step solid. */
static void gauge_on_side(const st_led_inputs_t *in, st_led_frame_t *out, bool blink_step)
{
	uint8_t level, i;

	for (i = 0; i < ST_LED_SIDE_COUNT; i++) {
		out->level[ST_LED_SIDE_FIRST + i] = 0u;
	}
	if (!batt_trustworthy(in)) {
		return;   /* never fabricate a level */
	}

	level = (in->batt_state == ST_LED_BATT_CHARGE_COMPLETE)
		? ST_LED_BATT_LEVEL_COUNT : in->batt_level;

	for (i = 0; i < ST_LED_SIDE_COUNT; i++) {
		uint8_t step = (uint8_t)(i + 1u);   /* S1 == step 1 */

		if (step < level) {
			out->level[ST_LED_SIDE_FIRST + i] = ST_LED_MAX;   /* completed */
		} else if (step == level) {
			out->level[ST_LED_SIDE_FIRST + i] =
				(blink_step && !in->batt_blink_on) ? 0u : ST_LED_MAX;
		}
	}
}

/* Linear fade from full to dark across ST_LED_SIDE_FADE_MS. */
static uint8_t fade_level(uint32_t into_fade_ms)
{
	uint32_t remain;

	if (into_fade_ms >= ST_LED_SIDE_FADE_MS) {
		return 0u;
	}
	remain = ST_LED_SIDE_FADE_MS - into_fade_ms;
	return (uint8_t)((remain * ST_LED_MAX) / ST_LED_SIDE_FADE_MS);
}

/*
 * 1. POWER-OFF. Side row flashes together, track row blinks once, side fades
 *    out, everything ends dark. main.c holds SYSTEM_OFF until this reports
 *    finished (sequence_ms >= ST_LED_SHUTDOWN_TOTAL_MS), so the animation is
 *    always complete before the GPIO levels latch.
 */
static void decide_shutdown(const st_led_inputs_t *in, st_led_frame_t *out)
{
	uint8_t i;
	uint32_t t = in->sequence_ms;

	all_dark(out);

	for (i = 0; i < ST_LED_TRACK_COUNT; i++) {
		out->level[i] = (t < ST_LED_TRACK_BLINK_MS) ? ST_LED_MAX : 0u;
	}
	for (i = 0; i < ST_LED_SIDE_COUNT; i++) {
		if (t < ST_LED_SIDE_HOLD_MS) {
			out->level[ST_LED_SIDE_FIRST + i] = ST_LED_MAX;
		} else {
			out->level[ST_LED_SIDE_FIRST + i] =
				fade_level(t - ST_LED_SIDE_HOLD_MS);
		}
	}
}

/*
 * 2. POWER-ON. Side row on together, track row blinks once, side fades, then
 *    the measured battery gauge appears briefly on S1..S4, then dark.
 */
static void decide_boot(const st_led_inputs_t *in, st_led_frame_t *out)
{
	uint8_t i;
	uint32_t t = in->sequence_ms;

	all_dark(out);

	/* Track row: one blink, then dark for the rest of the sequence. */
	for (i = 0; i < ST_LED_TRACK_COUNT; i++) {
		out->level[i] = (t < ST_LED_TRACK_BLINK_MS) ? ST_LED_MAX : 0u;
	}

	if (t < ST_LED_SIDE_HOLD_MS) {
		for (i = 0; i < ST_LED_SIDE_COUNT; i++) {
			out->level[ST_LED_SIDE_FIRST + i] = ST_LED_MAX;
		}
		return;
	}
	if (t < ST_LED_BOOT_FADE_END_MS) {
		uint8_t lv = fade_level(t - ST_LED_SIDE_HOLD_MS);

		for (i = 0; i < ST_LED_SIDE_COUNT; i++) {
			out->level[ST_LED_SIDE_FIRST + i] = lv;
		}
		return;
	}
	/* Battery preview: solid gauge, no blink, and dark if untrustworthy. */
	gauge_on_side(in, out, /*blink_step=*/false);
}

/*
 * 4. IMMEDIATE MOMENTARY SOLO. Only the held stem's Track LED, at maximum --
 *    never faint, never ghosted -- and every other Track LED completely off.
 *    This overrides the beat/chase display entirely; it does not blend with
 *    it. The side row is left to the caller's playing/charging decision so
 *    S4 can keep showing tempo while a stem is soloed.
 */
static void decide_solo_tracks(const st_led_inputs_t *in, st_led_frame_t *out)
{
	uint8_t i;

	for (i = 0; i < ST_LED_TRACK_COUNT; i++) {
		out->level[i] = (i == in->solo_index) ? ST_LED_MAX : 0u;
	}
}

/*
 * 5. PLAYING. One shared beat envelope drives all four Track LEDs and S4, so
 *    they cannot drift apart -- there is a single st_beat_pulse() result and
 *    every light here is derived from it.
 *
 *    Per stem: envelope scaled by that stem's activity, so a loud stem
 *    punches and a quiet one is dim -- but ONLY inside the pulse window.
 *    Activity never gates or times anything; between beats everything is
 *    dark regardless of how loud the audio is. That is the difference
 *    between this and the free-running VU display it replaces.
 *
 *    Bar position: the beat's own Track LED gets the full envelope
 *    regardless of activity, so the 1->2->3->4 chase stays readable even
 *    when that stem is silent.
 */
static uint8_t scale8(uint8_t a, uint8_t b)
{
	return (uint8_t)(((uint16_t)a * (uint16_t)b) / 255u);
}

static void decide_playing(const st_led_inputs_t *in, st_led_frame_t *out)
{
	uint8_t i;
	uint8_t env;

	all_dark(out);

	if (!in->beat.valid || !in->beat.in_pulse) {
		return;   /* between pulses, or no trustworthy tempo: dark */
	}
	env = in->beat.envelope;

	for (i = 0; i < ST_LED_TRACK_COUNT; i++) {
		uint8_t lv = scale8(env, in->stem_activity[i]);

		if (i == in->beat.beat_index) {
			/* CHASE ACCENT: full envelope, never dimmed by a quiet
			 * stem -- the bar position has to stay readable. */
			lv = env;
		}
		out->level[i] = lv;
	}

	/* S4 shares the SAME envelope value: playing is shown on the side row
	 * with the identical pulse, not a second animation. S1..S3 stay dark
	 * during playback -- playback outranks the charging gauge. */
	out->level[ST_LED_S4] = env;
}

void st_led_mvp_decide(const st_led_inputs_t *in, st_led_frame_t *out)
{
	uint8_t i;

	/* 1. POWER-OFF outranks everything. */
	if (in->sequence == ST_LED_SEQ_SHUTDOWN) {
		decide_shutdown(in, out);
		return;
	}

	/* 2. POWER-ON. */
	if (in->sequence == ST_LED_SEQ_BOOT) {
		decide_boot(in, out);
		return;
	}

	/* 3. TRANSFER: all four Track LEDs blink together. The side row shows
	 *    no fabricated status -- it stays dark, because a transfer says
	 *    nothing about tempo and the charging gauge would be misread as
	 *    transfer progress. */
	if (in->transfer_active) {
		all_dark(out);
		for (i = 0; i < ST_LED_TRACK_COUNT; i++) {
			out->level[i] = in->transfer_blink_on ? ST_LED_MAX : 0u;
		}
		return;
	}

	/* 5/6/8 first, so the side row is decided; solo then overrides only
	 *    the track row, which is what lets S4 keep pulsing under a solo. */
	if (in->playing && in->song_selected) {
		decide_playing(in, out);
	} else if (in->batt_state == ST_LED_BATT_CHARGING ||
		   in->batt_state == ST_LED_BATT_CHARGE_COMPLETE) {
		/* 6. CHARGING GAUGE, only while not playing. Completed steps
		 *    solid, the current step blinking, all four solid at full.
		 *
		 *    Blink ONLY while actually charging. CHARGE_COMPLETE reaches
		 *    this branch too, and passing blink unconditionally left its
		 *    top step dark on the off-phase -- so "fully charged" showed
		 *    three solid LEDs and a blinking fourth instead of four
		 *    solid, which reads as "still charging". */
		all_dark(out);
		gauge_on_side(in, out,
			      /*blink_step=*/(in->batt_state == ST_LED_BATT_CHARGING));
	} else {
		/* 8. IDLE: everything dark. No standby chase, no song-bank
		 *    display, and a merely-selected song does not light the
		 *    track row. */
		all_dark(out);
	}

	/* 4. IMMEDIATE SOLO overrides the track row, at any transport state. */
	if (in->solo_held && in->solo_index < ST_LED_TRACK_COUNT) {
		decide_solo_tracks(in, out);
	}
}
