/*
 * led_frame.c — see led_frame.h. PURE: no Zephyr, no hardware.
 */

#include "led_frame.h"

#define LED_STAGED_MASK_FULL ((uint16_t)((1u << LED_PHYSICAL_COUNT) - 1u))

int led_seq_compare(uint8_t seq_a, uint8_t seq_b)
{
	uint8_t diff = (uint8_t)((uint32_t)(seq_a - seq_b) & (LED_SEQ_MODULUS - 1u));

	if (diff == 0u) {
		return 0;
	}
	return (diff < (LED_SEQ_MODULUS / 2u)) ? 1 : -1;
}

void led_frame_reset(led_frame_state_t *s)
{
	uint8_t i;

	for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
		s->staged[i] = 0u;
		s->active[i] = 0u;
	}
	s->staged_mask = 0u;
	s->owned = false;
	s->had_commit = false;
	s->last_seq = 0u;
	s->lease_deadline_ms = 0u;

	s->valid_commits = 0u;
	s->rejected_commits = 0u;
	s->duplicate_commits = 0u;
	s->lease_timeouts = 0u;
	s->explicit_releases = 0u;
	s->disconnect_releases = 0u;
}

void led_frame_stage(led_frame_state_t *s, uint8_t index, uint8_t level)
{
	if (index >= LED_PHYSICAL_COUNT) {
		return;
	}
	if (level > LED_LEVEL_MAX) {
		level = LED_LEVEL_MAX;
	}
	s->staged[index] = level;
	s->staged_mask = (uint16_t)(s->staged_mask | (1u << index));
	/* `active` (and therefore the physical output) is untouched: staging
	 * must never change visible outputs. */
}

bool led_frame_all_staged(const led_frame_state_t *s)
{
	return s->staged_mask == LED_STAGED_MASK_FULL;
}

static void led_frame_apply_lease(led_frame_state_t *s, uint32_t now_ms)
{
	s->lease_deadline_ms = now_ms + LED_LEASE_TIMEOUT_MS;
}

led_commit_result_t led_frame_commit(led_frame_state_t *s, uint8_t seq, uint32_t now_ms)
{
	uint8_t i;

	if (!s->owned) {
		/* "The first commit after boot, MIDI connection, release, or
		 * timeout is accepted only after all eight indices have been
		 * staged." */
		if (!led_frame_all_staged(s)) {
			s->rejected_commits++;
			return LED_COMMIT_REJECTED_INCOMPLETE;
		}
		for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
			s->active[i] = s->staged[i];
		}
		s->owned = true;
		s->had_commit = true;
		s->last_seq = seq;
		led_frame_apply_lease(s, now_ms);
		s->valid_commits++;
		return LED_COMMIT_ACCEPTED;
	}

	/* Already owned: a later commit may update only a subset (staged[]
	 * already carries every prior value for the untouched indices), but
	 * "commit copies the complete staged frame to the active frame". */
	switch (led_seq_compare(seq, s->last_seq)) {
	case 0:
		/* Duplicate commit: idempotent no-op on `active`, but it is
		 * still host-liveness evidence, so the lease is refreshed. */
		s->duplicate_commits++;
		led_frame_apply_lease(s, now_ms);
		return LED_COMMIT_ACCEPTED_DUPLICATE;
	case 1:
		for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
			s->active[i] = s->staged[i];
		}
		s->last_seq = seq;
		led_frame_apply_lease(s, now_ms);
		s->valid_commits++;
		return LED_COMMIT_ACCEPTED;
	default:
		/* Stale/out-of-order: no state change, no lease refresh — an
		 * old or spoofed sequence must not resuscitate the lease. */
		s->rejected_commits++;
		return LED_COMMIT_REJECTED_STALE;
	}
}

led_heartbeat_result_t led_frame_heartbeat(led_frame_state_t *s, uint8_t seq, uint32_t now_ms)
{
	/* `seq` is accepted (it carries "the most recently committed
	 * sequence" per the protocol) but deliberately not validated: a
	 * heartbeat's only job is liveness. Rejecting it on a mismatch would
	 * risk dropping the lease early — the opposite of this module's
	 * fail-safe goal. */
	(void)seq;
	if (!s->owned) {
		/* "Heartbeat never begins ownership by itself." */
		return LED_HEARTBEAT_IGNORED_NOT_OWNED;
	}
	led_frame_apply_lease(s, now_ms);
	return LED_HEARTBEAT_EXTENDED;
}

bool led_frame_check_lease_timeout(led_frame_state_t *s, uint32_t now_ms)
{
	if (!s->owned) {
		return false;
	}
	if (now_ms < s->lease_deadline_ms) {
		return false;
	}
	led_frame_release(s, LED_RELEASE_TIMEOUT, now_ms);
	return true;
}

void led_frame_release(led_frame_state_t *s, led_release_reason_t reason, uint32_t now_ms)
{
	uint8_t i;

	(void)now_ms;
	s->owned = false;
	s->staged_mask = 0u; /* a fresh complete frame is required before takeover resumes */
	for (i = 0; i < LED_PHYSICAL_COUNT; i++) {
		s->active[i] = 0u; /* turn off the runtime frame */
	}
	switch (reason) {
	case LED_RELEASE_EXPLICIT:
		s->explicit_releases++;
		break;
	case LED_RELEASE_TIMEOUT:
		s->lease_timeouts++;
		break;
	case LED_RELEASE_DISCONNECT:
		s->disconnect_releases++;
		break;
	case LED_RELEASE_REINIT:
	default:
		break; /* silent state clear: not a host misbehavior */
	}
}

led_render_source_t led_render_select(bool safety_active, bool host_owned)
{
	if (safety_active) {
		return LED_RENDER_SOURCE_PATTERN;
	}
	return host_owned ? LED_RENDER_SOURCE_HOST : LED_RENDER_SOURCE_PATTERN;
}
