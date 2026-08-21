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

/* The four-step gauge on S1..S4, bottom-up. THE side-row display for every
 * powered-on, not-playing state -- boot preview, idle, stopped-with-a-song,
 * transfer, charging and full alike. `blink_step` blinks the current (topmost
 * lit) step, which only the actively-CHARGING display wants; every other
 * caller passes false and shows every step solid.
 *
 * Untrustworthy readings leave the side row dark and return: an unavailable,
 * faulted or never-seeded gauge is never rendered as a charge level. */
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
 *    the measured battery gauge appears on S1..S4 -- and STAYS. When the boot
 *    sequence ends, the stopped state draws the identical gauge, so the
 *    handover is invisible; ST_LED_BATT_PREVIEW_MS is now just the tail of
 *    the boot animation, not a timed window after which the row goes dark.
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
 * 4. IMMEDIATE MOMENTARY SOLO, ONE OR SEVERAL TRACKS. Every held Track LED
 *    at maximum -- never faint, never ghosted -- and every Track LED that is
 *    NOT held completely off. This overrides the beat/chase display
 *    entirely; it does not blend with it. The side row is left to the
 *    caller's playing/stopped decision so S4 can keep showing tempo, or the
 *    battery gauge can stay lit, while stems are soloed.
 *
 *    A chord is not a special case here: the mask simply has more bits set,
 *    and adding or removing a finger changes exactly the corresponding LED
 *    while the others hold. That falls out of assigning all four from the
 *    mask on every call rather than tracking "which one" is soloed.
 */
static void decide_solo_tracks(const st_led_inputs_t *in, st_led_frame_t *out)
{
	uint8_t i;

	for (i = 0; i < ST_LED_TRACK_COUNT; i++) {
		out->level[i] = ((in->solo_mask >> i) & 1u) ? ST_LED_MAX : 0u;
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

/*
 * THE FX OVERLAY DISPLAY. Track row = which effects are sounding; side row =
 * where the one rack is. Ranked above solo and the loop chase because while
 * the overlay is open those buttons no longer mean solo.
 */
static void decide_fx(const st_led_inputs_t *in, st_led_frame_t *out)
{
	const uint8_t env = (in->beat.valid && in->beat.in_pulse)
			    ? in->beat.envelope : 0u;
	uint8_t i;

	all_dark(out);

	for (i = 0; i < ST_LED_TRACK_COUNT; i++) {
		const uint8_t bit = (uint8_t)(1u << i);

		if ((in->fx_latched & bit) != 0u) {
			/* SOLID: still sounding when the finger is off, and it
			 * survives the overlay closing. */
			out->level[i] = ST_LED_MAX;
		} else if ((in->fx_momentary & bit) != 0u) {
			/* BREATHING on the shared beat envelope -- live, ends
			 * when released. A momentary with no trustworthy tempo
			 * still has to be visible, so it floors at half. */
			out->level[i] = (env > 128u) ? env : 128u;
		}
	}

	if (in->fx_global) {
		/* The rack is on everything: the whole side row, dimmer than a
		 * single selected stem so the two never read the same. */
		for (i = 0; i < ST_LED_SIDE_COUNT; i++) {
			out->level[ST_LED_SIDE_FIRST + i] = 120u;
		}
	} else {
		uint8_t t = (in->fx_target < ST_LED_SIDE_COUNT) ? in->fx_target : 0u;

		out->level[ST_LED_SIDE_FIRST + t] = ST_LED_MAX;
	}
}

static void decide_playing(const st_led_inputs_t *in, st_led_frame_t *out)
{
	uint8_t i;
	uint8_t env;
	/*
	 * THE LOOP CHASE. While a loop is running -- momentary or latched --
	 * the Track row stops being a beat pulse and becomes a position
	 * display: exactly one LED at full brightness, advancing
	 * T1 -> T2 -> T3 -> T4 in tempo, T1 on the downbeat.
	 *
	 * It is HELD rather than pulsed, which is the whole difference: a
	 * pulse says "a beat happened", a held light says "you are here". A
	 * looping player needs the second, and needs it visible between beats.
	 *
	 * NO SECOND CLOCK, and no new timing state anywhere. beat_index comes
	 * from the same single st_beat_pulse() call every other light here is
	 * derived from, computed on the authoritative song_frame -- which,
	 * while a loop is running, IS the loop playback frame -- against the
	 * selected STIX record's own bpm_q8 and downbeat_frame. beat_index and
	 * valid are set independently of in_pulse (st_beat_phase.c), which is
	 * why holding through the gaps needs nothing added to that module.
	 *
	 * WITHOUT A LOOP NOTHING HERE APPLIES. The pulse-and-accent display
	 * below is physically verified and is left exactly as it is.
	 */
	const bool loop_chase = (in->loop_state != ST_LED_LOOP_NONE) &&
				 in->beat.valid;

	all_dark(out);

	/* THE LOOP MARKER, on S1 and nowhere else. A LATCHED loop is solid, so
	 * it is visible even between beats -- which is the point: a latched
	 * loop is still running when the player's hands are off the device. A
	 * MOMENTARY loop only ever shows inside the pulse, below, so it reads
	 * as "live" rather than "held".
	 *
	 * PHYSICALLY VERIFIED on a real SP-1 and NOT to be redesigned: an
	 * earlier revision of this checkpoint replaced this with a Track-row
	 * quarter chase. That is reverted -- the shipped indication works. */
	if (in->loop_state == ST_LED_LOOP_LATCHED) {
		out->level[ST_LED_S1] = ST_LED_MAX;
	}

	/* Set BEFORE the between-pulses return, because the chase is the one
	 * thing on this row that stays lit between beats. */
	if (loop_chase) {
		for (i = 0; i < ST_LED_TRACK_COUNT; i++) {
			out->level[i] = (i == in->beat.beat_index)
					? ST_LED_MAX : 0u;
		}
	}

	if (!in->beat.valid || !in->beat.in_pulse) {
		return;   /* between pulses, or no trustworthy tempo: dark */
	}
	env = in->beat.envelope;

	if (in->loop_state == ST_LED_LOOP_MOMENTARY) {
		/* The SAME envelope value the Track row and S4 carry -- one
		 * shared beat, not a third animation with its own timing. */
		out->level[ST_LED_S1] = env;
	}

	if (!loop_chase) {
		for (i = 0; i < ST_LED_TRACK_COUNT; i++) {
			uint8_t lv = scale8(env, in->stem_activity[i]);

			if (i == in->beat.beat_index) {
				/* CHASE ACCENT: full envelope, never dimmed by
				 * a quiet stem -- the bar position has to stay
				 * readable. */
				lv = env;
			}
			out->level[i] = lv;
		}
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

	/* 3. TRANSFER: all four Track LEDs blink together. The side row is NOT
	 *    part of the transfer display -- it keeps showing the battery
	 *    gauge, because ADC sampling is deliberately paused during a
	 *    transfer and the gauge is sticky, so what is shown is the last
	 *    trustworthy reading rather than a stale guess. A charging step
	 *    keeps blinking through the transfer for the same reason.
	 *
	 *    (This is a correction: the side row used to go dark here, on the
	 *    reasoning that a charge gauge might be misread as transfer
	 *    progress. The product rule is that a powered-on, not-playing SP-1
	 *    always shows its battery level, and a transfer is exactly such a
	 *    state -- the four blinking Track LEDs are the transfer indication
	 *    and are not ambiguous with a static side-row gauge.) */
	if (in->transfer_active) {
		all_dark(out);
		for (i = 0; i < ST_LED_TRACK_COUNT; i++) {
			out->level[i] = in->transfer_blink_on ? ST_LED_MAX : 0u;
		}
		gauge_on_side(in, out,
			      /*blink_step=*/(in->batt_state == ST_LED_BATT_CHARGING));
		return;
	}

	/* 3b. THE FX OVERLAY, ranked between transfer and solo.
	 *
	 *     While the overlay is open the Track buttons are effects, not
	 *     solos, so showing solo feedback here would be showing something
	 *     the buttons no longer do. It therefore returns rather than
	 *     falling through -- and because it owns no clock, closing the
	 *     overlay restores the underlying mode on the very next frame with
	 *     the beat phase and loop phase untouched. */
	if (in->fx_open) {
		decide_fx(in, out);
		return;
	}

	/* THE SIDE ROW HAS EXACTLY TWO STATES, and transport alone picks which:
	 *
	 *    playing      -> S1..S3 dark, S4 carries the beat envelope.
	 *    not playing  -> the four-step battery gauge, continuously.
	 *
	 * There is no third "idle, therefore dark" case. A powered-on SP-1 that
	 * is not playing shows its charge level -- with no song selected, with a
	 * song selected but stopped, on battery, on USB, charging, full, or with
	 * a Track button held. The ONLY thing that darkens the side row while
	 * stopped is a reading the gauge itself refuses to trust, which
	 * gauge_on_side() handles by leaving it dark rather than inventing a
	 * level.
	 *
	 * Decided BEFORE solo so that solo overrides the track row only, which
	 * is what lets S4 keep pulsing (playing) or the gauge stay lit
	 * (stopped) while a stem is soloed. */
	if (in->playing && in->song_selected) {
		decide_playing(in, out);
	} else {
		all_dark(out);
		/* Blink ONLY while actually charging. CHARGE_COMPLETE renders
		 * all four solid (gauge_on_side() forces the level to full);
		 * blinking its top step would read as "still charging", and
		 * CHARGER_ABSENT/LOW render their measured level continuously. */
		gauge_on_side(in, out,
			      /*blink_step=*/(in->batt_state == ST_LED_BATT_CHARGING));
	}

	/* 4. IMMEDIATE SOLO overrides the track row, at any transport state.
	 *    Any nonzero mask means at least one Track is physically down. */
	if ((in->solo_mask & ST_LED_TRACK_MASK_ALL) != 0u) {
		decide_solo_tracks(in, out);
	}
}
