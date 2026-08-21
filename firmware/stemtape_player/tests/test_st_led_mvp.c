/*
 * test_st_led_mvp.c — the Stem Tape MVP LED acceptance matrix.
 *
 * This drives the REAL production decision, st_led_mvp_decide(), the same
 * function firmware/stemtape_player/src/main.c's led_service() calls on the
 * device. It is not a reference implementation, not a parallel model, and
 * not a mock: the object file linked here is the object file linked into
 * the firmware.
 *
 * Audibility is likewise the real thing: st_stem_mix_channel_audible() from
 * st_stem_mix.c, the SAME function the audio thread applies to its own
 * channel array. So a mute/solo case that passes here passes because the
 * mixer agrees a stem is silent, not because this test decided it should be.
 *
 * WHAT THIS PROVES: that given a described runtime state, the production
 * decision assigns the eight LEDs the modes the contract and the product
 * owner's MVP decisions call for.
 *
 * WHAT IT DOES NOT PROVE: that the physical device lights up that way. No
 * GPIO, no TIMER3, no eye. The renderer main.c applies these modes with is
 * proven separately (it is the unchanged, already-working soft-PWM driver),
 * and the physical check remains a human one.
 *
 *   cc -std=c11 -Wall -Wextra -I../src ../src/st_led_mvp.c ../src/st_stem_mix.c \
 *      test_st_led_mvp.c -o test_st_led_mvp && ./test_st_led_mvp
 */

#include <stdio.h>
#include <string.h>

#include "st_led_mvp.h"
#include "st_stem_mix.h"

static int g_checks, g_failures, g_cases;

#define CHECK(cond, ...) do { \
		g_checks++; \
		if (cond) { printf("[OK  ] " __VA_ARGS__); printf("\n"); } \
		else { g_failures++; printf("[FAIL] " __VA_ARGS__); \
		       printf("  (%s:%d: %s)\n", __FILE__, __LINE__, #cond); } \
	} while (0)

static const char *mode_name(uint8_t m)
{
	switch (m) {
	case ST_LED_OFF:   return "off";
	case ST_LED_GHOST: return "faint";
	case ST_LED_SOLID: return "SOLID";
	default:           return "?";
	}
}

/* Print one row of the eight-LED state table exactly as the report quotes it. */
static void show(const char *label, const st_led_frame_t *f)
{
	printf("      %-34s T1=%-5s T2=%-5s T3=%-5s T4=%-5s | PLAY=%-5s B2=%-5s B3=%-5s B4=%-5s\n",
	       label,
	       mode_name(f->mode[0]), mode_name(f->mode[1]),
	       mode_name(f->mode[2]), mode_name(f->mode[3]),
	       mode_name(f->mode[ST_LED_SIDE_TRANSPORT]),
	       mode_name(f->mode[ST_LED_SIDE_BATT_FIRST + 0]),
	       mode_name(f->mode[ST_LED_SIDE_BATT_FIRST + 1]),
	       mode_name(f->mode[ST_LED_SIDE_BATT_FIRST + 2]));
}

/* Build inputs the way main.c's led_service() does: four loaded stems, with
 * audibility taken from the REAL mixer rule rather than restated here. */
static void make_song(st_led_inputs_t *in, const bool muted[4], const bool solo[4])
{
	st_stem_mix_channel_t ch[ST11_STEM_COUNT];
	int i;

	memset(in, 0, sizeof(*in));
	in->song_selected = true;

	for (i = 0; i < (int)ST11_STEM_COUNT; i++) {
		ch[i].gain_q8 = ST_STEM_MIX_GAIN_UNITY_Q8;
		ch[i].mute = muted[i];
		ch[i].solo = solo[i];
	}
	for (i = 0; i < (int)ST_LED_TRACK_COUNT; i++) {
		in->stem_loaded[i]  = true;
		in->stem_audible[i] = st_stem_mix_channel_audible(ch, (uint32_t)i);
		in->stem_soloed[i]  = solo[i];
		if (solo[i]) {
			in->solo_active = true;
		}
	}
}

static const bool none[4] = { false, false, false, false };

/* A trustworthy mid-charge battery, so the battery LEDs are not the subject
 * of the transport/track cases. Level 3 of 4 -> two solid, per the gauge. */
static void batt_ok(st_led_inputs_t *in)
{
	in->batt_state = ST_LED_BATT_CHARGER_ABSENT;
	in->batt_level = 3u;
}

/* ===================== 1. valid song selected, stopped ==================== */
static void case_1_stopped(void)
{
	st_led_inputs_t in; st_led_frame_t f;

	g_cases++;
	printf("\n-- 1. Valid song selected, STOPPED\n");
	make_song(&in, none, none);
	batt_ok(&in);
	in.playing = false;
	st_led_mvp_decide(&in, &f);
	show("song selected, stopped", &f);

	CHECK(f.mode[ST_LED_SIDE_TRANSPORT] == ST_LED_OFF,
	      "transport LED (nearest PLAY) is OFF while stopped");
	for (int i = 0; i < 4; i++) {
		CHECK(f.mode[i] == ST_LED_SOLID,
		      "track %d is SOLID: loaded and audible", i + 1);
	}
	CHECK(f.mode[ST_LED_SIDE_BATT_FIRST] == ST_LED_SOLID &&
	      f.mode[ST_LED_SIDE_BATT_FIRST + 1] == ST_LED_SOLID &&
	      f.mode[ST_LED_SIDE_BATT_FIRST + 2] == ST_LED_OFF,
	      "battery level 3 of 4 lights two of the three battery LEDs");
}

/* ============================== 2. playing =============================== */
static void case_2_playing(void)
{
	st_led_inputs_t in; st_led_frame_t f;

	g_cases++;
	printf("\n-- 2. PLAYING\n");
	make_song(&in, none, none);
	batt_ok(&in);
	in.playing = true;
	st_led_mvp_decide(&in, &f);
	show("playing", &f);

	CHECK(f.mode[ST_LED_SIDE_TRANSPORT] == ST_LED_SOLID,
	      "transport LED is steadily illuminated while playing");
	for (int i = 0; i < 4; i++) {
		CHECK(f.mode[i] == ST_LED_SOLID,
		      "track %d stays SOLID while playing -- stable and readable, "
		      "not a level meter that dims on a quiet passage", i + 1);
	}
}

/* ==================== 3. track 1 muted, then unmuted ===================== */
static void case_3_mute(void)
{
	st_led_inputs_t in; st_led_frame_t f;
	bool muted[4] = { true, false, false, false };

	g_cases++;
	printf("\n-- 3. Track 1 MUTED, then unmuted\n");
	make_song(&in, muted, none);
	batt_ok(&in);
	in.playing = true;
	st_led_mvp_decide(&in, &f);
	show("track 1 muted", &f);

	CHECK(f.mode[0] == ST_LED_GHOST,
	      "muted track 1 is FAINT -- distinguishable from an empty lane, which is off");
	CHECK(f.mode[1] == ST_LED_SOLID && f.mode[2] == ST_LED_SOLID && f.mode[3] == ST_LED_SOLID,
	      "the other three stay SOLID");

	muted[0] = false;
	make_song(&in, muted, none);
	batt_ok(&in);
	in.playing = true;
	st_led_mvp_decide(&in, &f);
	show("track 1 unmuted", &f);
	CHECK(f.mode[0] == ST_LED_SOLID, "unmuting restores track 1 to SOLID");
}

/* ============= 4. track 2 held for momentary solo, then released ========= */
static void case_4_solo(void)
{
	st_led_inputs_t in; st_led_frame_t f;
	bool solo[4] = { false, true, false, false };

	g_cases++;
	printf("\n-- 4. Track 2 HELD for momentary solo, then released\n");
	make_song(&in, none, solo);
	batt_ok(&in);
	in.playing = true;
	st_led_mvp_decide(&in, &f);
	show("track 2 solo held", &f);

	CHECK(f.mode[1] == ST_LED_SOLID, "the soloed stem is SOLID");
	CHECK(f.mode[0] == ST_LED_GHOST && f.mode[2] == ST_LED_GHOST && f.mode[3] == ST_LED_GHOST,
	      "stems silenced by the solo are FAINT -- the contract's "
	      "\"soloed stem solid, non-solo stems faint\"");

	/* Release. */
	make_song(&in, none, none);
	batt_ok(&in);
	in.playing = true;
	st_led_mvp_decide(&in, &f);
	show("solo released", &f);
	for (int i = 0; i < 4; i++) {
		CHECK(f.mode[i] == ST_LED_SOLID,
		      "release immediately restores track %d to its underlying audible state",
		      i + 1);
	}
}

/* ================== 5. transfer begins, then completes =================== */
static void case_5_transfer(void)
{
	st_led_inputs_t in; st_led_frame_t f;
	bool muted[4] = { true, false, false, false };

	g_cases++;
	printf("\n-- 5. TRANSFER begins, then completes\n");

	/* Mute track 1 BEFORE the transfer, so "restores live state" is a real
	 * claim: if the overlay restored a snapshot it would have to restore
	 * this, and if it restored a default it would lose it. */
	make_song(&in, muted, none);
	batt_ok(&in);
	in.playing = false;
	in.transfer_active = true;
	in.transfer_blink_on = true;
	st_led_mvp_decide(&in, &f);
	show("transfer, blink on", &f);
	CHECK(f.mode[0] == ST_LED_SOLID && f.mode[1] == ST_LED_SOLID &&
	      f.mode[2] == ST_LED_SOLID && f.mode[3] == ST_LED_SOLID,
	      "all four track LEDs blink TOGETHER (on phase)");

	in.transfer_blink_on = false;
	st_led_mvp_decide(&in, &f);
	show("transfer, blink off", &f);
	CHECK(f.mode[0] == ST_LED_OFF && f.mode[1] == ST_LED_OFF &&
	      f.mode[2] == ST_LED_OFF && f.mode[3] == ST_LED_OFF,
	      "all four track LEDs are off together (off phase)");
	CHECK(f.mode[ST_LED_SIDE_BATT_FIRST] == ST_LED_SOLID,
	      "the side row is untouched by the transfer overlay -- battery stays readable");

	/* Complete. */
	in.transfer_active = false;
	st_led_mvp_decide(&in, &f);
	show("transfer complete", &f);
	CHECK(f.mode[0] == ST_LED_GHOST,
	      "normal state restores IMMEDIATELY, and restores the LIVE mute state "
	      "(track 1 still muted), not a snapshot and not a default");
	CHECK(f.mode[1] == ST_LED_SOLID && f.mode[2] == ST_LED_SOLID && f.mode[3] == ST_LED_SOLID,
	      "the unmuted stems return to SOLID");
}

/* =================== 6. playback stops and restarts ====================== */
static void case_6_stop_restart(void)
{
	st_led_inputs_t in; st_led_frame_t f;
	st_led_frame_t first;

	g_cases++;
	printf("\n-- 6. Playback STOPS and RESTARTS\n");
	make_song(&in, none, none);
	batt_ok(&in);
	in.playing = true;
	st_led_mvp_decide(&in, &first);
	show("playing", &first);

	in.playing = false;
	st_led_mvp_decide(&in, &f);
	show("stopped", &f);
	CHECK(f.mode[ST_LED_SIDE_TRANSPORT] == ST_LED_OFF, "transport LED goes off on stop");

	in.playing = true;
	st_led_mvp_decide(&in, &f);
	show("restarted", &f);
	CHECK(memcmp(&f, &first, sizeof(f)) == 0,
	      "restarting reproduces the playing frame EXACTLY -- the decision holds no "
	      "state of its own, so there is no stale meter value to carry over");
}

/* ================= 7. USB disconnect after the transfer ================== */
static void case_7_usb_disconnect(void)
{
	st_led_inputs_t in; st_led_frame_t f;

	g_cases++;
	printf("\n-- 7. USB DISCONNECT after transfer\n");
	make_song(&in, none, none);
	in.playing = false;
	/* Unplugged: charger gone. The gauge keeps its last sticky level, so the
	 * classifier reports ordinary battery operation. */
	in.batt_state = ST_LED_BATT_CHARGER_ABSENT;
	in.batt_level = 3u;
	in.transfer_active = false;
	st_led_mvp_decide(&in, &f);
	show("usb disconnected", &f);

	CHECK(f.mode[0] == ST_LED_SOLID && f.mode[3] == ST_LED_SOLID,
	      "the track row returns to normal state -- no transfer residue");
	CHECK(f.mode[ST_LED_SIDE_BATT_FIRST] == ST_LED_SOLID &&
	      f.mode[ST_LED_SIDE_BATT_FIRST + 1] == ST_LED_SOLID,
	      "battery reverts to the local gauge, never to all-off");
}

/* ====================== 8. no valid song selected ======================== */
static void case_8_no_song(void)
{
	st_led_inputs_t in; st_led_frame_t f;

	g_cases++;
	printf("\n-- 8. NO valid song selected\n");
	memset(&in, 0, sizeof(in));
	batt_ok(&in);
	st_led_mvp_decide(&in, &f);
	show("no song", &f);

	for (int i = 0; i < 4; i++) {
		CHECK(f.mode[i] == ST_LED_OFF,
		      "track %d is OFF -- no standby chase, ever", i + 1);
	}
	CHECK(f.mode[ST_LED_SIDE_TRANSPORT] == ST_LED_OFF, "transport LED off with no song");
	CHECK(f.mode[ST_LED_SIDE_BATT_FIRST] == ST_LED_SOLID,
	      "battery is still shown -- it does not depend on a song");
}

/* ==================== 9. battery information unavailable ================= */
static void case_9_batt_unavailable(void)
{
	st_led_inputs_t in; st_led_frame_t f;

	g_cases++;
	printf("\n-- 9. Battery information UNAVAILABLE / FAULT\n");

	make_song(&in, none, none);
	in.playing = true;
	in.batt_state = ST_LED_BATT_UNAVAILABLE;
	in.batt_level = 0u;
	st_led_mvp_decide(&in, &f);
	show("battery unavailable", &f);
	CHECK(f.mode[ST_LED_SIDE_BATT_FIRST] == ST_LED_OFF &&
	      f.mode[ST_LED_SIDE_BATT_FIRST + 1] == ST_LED_OFF &&
	      f.mode[ST_LED_SIDE_BATT_FIRST + 2] == ST_LED_OFF,
	      "no trustworthy reading -> battery LEDs are DARK, not a guessed level");
	CHECK(f.mode[ST_LED_SIDE_TRANSPORT] == ST_LED_SOLID,
	      "transport is unaffected by the battery being unknown");

	in.batt_state = ST_LED_BATT_FAULT;
	in.batt_level = 3u;   /* a stale level exists, but the state is not trusted */
	st_led_mvp_decide(&in, &f);
	show("battery fault", &f);
	CHECK(f.mode[ST_LED_SIDE_BATT_FIRST] == ST_LED_OFF,
	      "FAULT is dark too -- a charger fault is never rendered as a charge level, "
	      "and never as an error code");

	/* The classifier's own safety rule, checked through the real function. */
	{
		st_led_batt_gauge_t g;

		st_led_batt_reset(&g);
		CHECK(st_led_batt_classify(&g, false, false) == ST_LED_BATT_UNAVAILABLE,
		      "a gauge that has never been seeded classifies UNAVAILABLE, not LOW");
		CHECK(st_led_batt_classify(&g, false, true) == ST_LED_BATT_FAULT,
		      "charging asserted with no power present is a FAULT, not LOW");

		st_led_batt_update(&g, true, 1900);   /* below THR_1 -> level 1 */
		CHECK(g.level == 1u, "a low raw reading seeds gauge level 1");
		CHECK(st_led_batt_classify(&g, false, false) == ST_LED_BATT_LOW,
		      "a VALID low reading does classify LOW");
		st_led_batt_update(&g, false, -1);    /* failed read after a good one */
		CHECK(st_led_batt_classify(&g, false, false) == ST_LED_BATT_FAULT,
		      "a failed read after a good one is FAULT, and the level stays sticky");
		CHECK(g.level == 1u, "the sticky level survives a failed read");
	}
}

/* ============================ 10. charging =============================== */
static void case_10_charging(void)
{
	st_led_inputs_t in; st_led_frame_t f;

	g_cases++;
	printf("\n-- 10. CHARGING, and charge complete\n");

	make_song(&in, none, none);
	in.playing = false;
	in.batt_state = ST_LED_BATT_CHARGING;
	in.batt_level = 3u;

	in.batt_blink_on = true;
	st_led_mvp_decide(&in, &f);
	show("charging, blink on", &f);
	CHECK(f.mode[ST_LED_SIDE_BATT_FIRST] == ST_LED_SOLID,
	      "steps below the current level stay solid while charging");
	CHECK(f.mode[ST_LED_SIDE_BATT_FIRST + 1] == ST_LED_SOLID,
	      "the current level's LED is lit on the blink-on phase");

	in.batt_blink_on = false;
	st_led_mvp_decide(&in, &f);
	show("charging, blink off", &f);
	CHECK(f.mode[ST_LED_SIDE_BATT_FIRST + 1] == ST_LED_OFF,
	      "ONLY the current level's LED blinks -- the approved battery step");
	CHECK(f.mode[ST_LED_SIDE_BATT_FIRST] == ST_LED_SOLID,
	      "the step below does not blink with it");

	in.batt_state = ST_LED_BATT_CHARGE_COMPLETE;
	st_led_mvp_decide(&in, &f);
	show("charge complete", &f);
	CHECK(f.mode[ST_LED_SIDE_BATT_FIRST] == ST_LED_SOLID &&
	      f.mode[ST_LED_SIDE_BATT_FIRST + 1] == ST_LED_SOLID &&
	      f.mode[ST_LED_SIDE_BATT_FIRST + 2] == ST_LED_SOLID,
	      "charge complete lights every battery LED solid");

	/* Lowest level still readable. */
	in.batt_state = ST_LED_BATT_LOW;
	in.batt_level = 1u;
	st_led_mvp_decide(&in, &f);
	show("battery level 1 (low)", &f);
	CHECK(f.mode[ST_LED_SIDE_BATT_FIRST] == ST_LED_GHOST,
	      "level 1 shows the bottom battery LED FAINT -- distinguishable from "
	      "\"no information\", which is dark");
	CHECK(f.mode[ST_LED_SIDE_BATT_FIRST + 1] == ST_LED_OFF,
	      "and only that one");
}

/* ============ structural guarantees the whole design rests on =========== */
static void case_invariants(void)
{
	st_led_inputs_t in; st_led_frame_t f;

	g_cases++;
	printf("\n-- Invariants\n");

	/* Every LED assigned on every call: prefill with a poison value and
	 * confirm none of it survives. */
	memset(&in, 0, sizeof(in));
	memset(&f, 0xEE, sizeof(f));
	st_led_mvp_decide(&in, &f);
	{
		int assigned = 1;
		for (unsigned i = 0; i < ST_LED_COUNT; i++) {
			if (f.mode[i] > ST_LED_SOLID) {
				assigned = 0;
			}
		}
		CHECK(assigned,
		      "all eight LEDs are assigned on every call -- no LED can carry a "
		      "value over from a previous frame");
	}

	/* Purity: same inputs, same output, no hidden state. */
	{
		st_led_frame_t a, b;
		bool muted[4] = { false, true, false, true };

		make_song(&in, muted, none);
		in.playing = true;
		batt_ok(&in);
		st_led_mvp_decide(&in, &a);
		for (int n = 0; n < 50; n++) {
			st_led_mvp_decide(&in, &b);
		}
		CHECK(memcmp(&a, &b, sizeof(a)) == 0,
		      "the decision is pure: 50 further calls with identical inputs "
		      "produce an identical frame");
	}
}

int main(void)
{
	printf("STEM TAPE MVP LED ACCEPTANCE MATRIX\n");
	printf("driving the REAL production st_led_mvp_decide(), with audibility from "
	       "the REAL st_stem_mix_channel_audible()\n");
	printf("T1..T4 = track LEDs; PLAY = side LED nearest PLAY (transport); "
	       "B2..B4 = the three battery LEDs\n");

	case_1_stopped();
	case_2_playing();
	case_3_mute();
	case_4_solo();
	case_5_transfer();
	case_6_stop_restart();
	case_7_usb_disconnect();
	case_8_no_song();
	case_9_batt_unavailable();
	case_10_charging();
	case_invariants();

	printf("\n%s (%d cases, %d checks, %d failures)\n",
	       g_failures ? "LED ACCEPTANCE MATRIX FAILED" : "LED ACCEPTANCE MATRIX PASSED",
	       g_cases, g_checks, g_failures);
	printf("NOTE: this proves the production DECISION assigns these modes. It does NOT\n"
	       "      prove the physical device lights up this way -- no GPIO, no TIMER3,\n"
	       "      no eye. Physical LED verification remains a human check.\n");
	return g_failures ? 1 : 0;
}
