/*
 * st_pwr_idle.c -- see st_pwr_idle.h for what "idle" means, why playback is
 * not idle, and why the three power mechanisms are kept disjoint.
 */

#include "st_pwr_idle.h"

void st_pwr_idle_init(st_pwr_idle_t *i)
{
	i->last_active_ms = 0;
	i->seeded         = false;
	st_pwr_fader_reset(&i->fader);
}

bool st_pwr_idle_fader(st_pwr_idle_t *i, uint32_t idx, int32_t raw,
			int64_t now_ms)
{
	/* The SAME per-sample delta rule the hold uses -- st_pwr_hold.c owns it
	 * and it is host-tested there -- over a DIFFERENT, continuously sampled
	 * st_pwr_fader_t. See the header for why there are two. */
	return st_pwr_fader_sample(&i->fader, idx, raw, now_ms);
}

int64_t st_pwr_idle_tick(st_pwr_idle_t *i, bool transport_active,
			  bool activity, int64_t now_ms)
{
	/*
	 * SEED ON THE FIRST TICK, not in the initialiser, because the
	 * initialiser has no clock. Until the first tick there is no elapsed
	 * time, so a freshly woken device cannot inherit one.
	 */
	if (!i->seeded) {
		i->last_active_ms = now_ms;
		i->seeded         = true;
		return 0;
	}

	/*
	 * IN USE = the reel is turning OR a human is touching something.
	 *
	 * Both pin the clock at zero rather than pausing it, for the same
	 * reason the manual hold zeroes rather than banks: partial credit is
	 * how a timer ends up meaning something other than what it says. When
	 * use stops, a full ST_PWR_IDLE_MS of continuous non-use is required.
	 */
	if (transport_active || activity) {
		i->last_active_ms = now_ms;
		return 0;
	}

	return (now_ms > i->last_active_ms) ? (now_ms - i->last_active_ms) : 0;
}

/* ---- the governor ----------------------------------------------------- */

static void gov_init(st_pwr_gov_t *g, bool device_on)
{
	if (device_on) {
		st_pwr_init_on(&g->pwr);
	} else {
		st_pwr_init_off(&g->pwr);
	}
	st_pwr_idle_init(&g->idle);
}

void st_pwr_gov_init_off(st_pwr_gov_t *g) { gov_init(g, false); }
void st_pwr_gov_init_on(st_pwr_gov_t *g)  { gov_init(g, true); }

void st_pwr_gov_service(st_pwr_gov_t *g, const st_pwr_gov_in_t *in,
			 st_pwr_gov_out_t *out)
{
	st_pwr_in_t  hin;
	st_pwr_out_t hout;
	bool fader_moved, activity;

	/*
	 * ---- B and A: THE MANUAL CONTRACT, UNCHANGED AND UNAWARE ----------
	 *
	 * st_pwr_in_t is built here from physical facts only. transport_active
	 * is NOT copied into it -- the struct has no such field, and CI asserts
	 * that st_pwr_hold.c never mentions transport at all. That is the
	 * structural guarantee that a stuck or wrong transport state can never
	 * suppress, extend or fake the FUNCTION escape hatch.
	 */
	hin.fn_down     = in->fn_down;
	hin.ain0_active = in->ain0_active;
	hin.ain1_active = in->ain1_active;
	hin.fader_raw   = in->fader_raw;
	hin.fader_idx   = in->fader_idx;
	hin.now_ms      = in->now_ms;
	st_pwr_service(&g->pwr, &hin, &hout);

	/*
	 * ---- THE WAKE RESETS THE IDLE SYSTEM ------------------------------
	 *
	 * A device that has just come up owes a fresh five minutes. Whatever
	 * the idle clock read before the transition -- including anything it
	 * accumulated while the gate loop was running -- is discarded here, so
	 * no historical idle elapsed can leak into the session.
	 */
	if (hout.on_due) {
		st_pwr_idle_init(&g->idle);
	}

	/*
	 * ---- C: IDLE, COMPUTED FROM ITS OWN INPUTS ------------------------
	 *
	 * The idle fader detector is sampled on EVERY pass, unlike the hold's,
	 * because a fader moved with FUNCTION up is exactly the intentional
	 * activity that should keep the device awake.
	 *
	 * Note what is absent from `activity`: off_armed, function_consumed,
	 * combo state, gesture ownership, any feature flag, and the hold's
	 * elapsed time. The idle clock reads none of them, so a manual path
	 * stuck disarmed cannot disable the idle shutdown, and stale feature
	 * state cannot keep a stopped device awake.
	 */
	fader_moved = st_pwr_idle_fader(&g->idle, in->fader_idx, in->fader_raw,
					 in->now_ms);
	activity = in->fn_down || in->ain0_active || in->ain1_active ||
		    fader_moved;

	out->idle_elapsed_ms = st_pwr_idle_tick(&g->idle, in->transport_active,
						 activity, in->now_ms);
	out->in_use = in->transport_active || activity;

	/*
	 * IDLE ONLY SHUTS DOWN A DEVICE THAT IS ON. While the wake gate is
	 * running the device is already off, and the standby loop has its own
	 * battery-idle rule. This reads device_on -- the power STATE -- and
	 * never off_armed, which is the release-to-rearm guard and belongs to
	 * the manual path alone.
	 */
	out->idle_off_due = hout.device_on &&
			     st_pwr_idle_due(out->idle_elapsed_ms);

	/* ---- the two verdicts meet here, and nowhere else ----------------- */
	out->on_due            = hout.on_due;
	out->hold_elapsed_ms   = hout.elapsed_ms;
	out->off_due           = hout.off_due;
	out->off_armed         = hout.off_armed;
	out->other_active      = hout.other_active;
	out->began             = hout.began;
	out->device_on         = hout.device_on;
	out->power_off_request = hout.off_due || out->idle_off_due;
}
