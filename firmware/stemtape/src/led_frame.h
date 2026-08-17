/*
 * led_frame.h — Stem Tape LED Feedback Protocol v1: staged/committed frame
 * state machine and host-ownership lease.
 *
 * PURE: no Zephyr headers, no hardware access, no I/O. Every input (level,
 * sequence, "now") is a plain argument; every output is a return value or a
 * field of led_frame_state_t. This is what makes it host-runnable: the exact
 * same translation unit links into the firmware (called from main.c with
 * k_uptime_get()) and into firmware/stemtape/tests/test_led.c (called with a
 * fake clock), so the test proves the real logic, not a reimplementation of
 * it.
 *
 * The website owns all gesture/loop/scrub/FX/mixer/precedence semantics and
 * sends an already-resolved 8-channel brightness frame; this module only
 * implements the transport-level contract in
 * docs/stem-tape-led-feedback-v1.md: stage, atomic commit, modulo-128
 * sequence freshness, and a 1000 ms lease. It never inspects *why* a level
 * is what it is.
 */

#ifndef STEMTAPE_LED_FRAME_H_
#define STEMTAPE_LED_FRAME_H_

#include <stdbool.h>
#include <stdint.h>

#include "led_protocol.h"

typedef enum {
	LED_COMMIT_ACCEPTED,            /* staged frame copied to active, lease (re)armed */
	LED_COMMIT_ACCEPTED_DUPLICATE,  /* seq == last committed seq: idempotent no-op */
	LED_COMMIT_REJECTED_INCOMPLETE, /* first commit since reset, but not all 8 staged */
	LED_COMMIT_REJECTED_STALE,      /* seq is not newer than the last committed seq */
} led_commit_result_t;

typedef enum {
	LED_HEARTBEAT_EXTENDED,          /* ownership active: lease deadline pushed out */
	LED_HEARTBEAT_IGNORED_NOT_OWNED, /* no active ownership: heartbeat is a no-op */
} led_heartbeat_result_t;

/* Why led_frame_release() was called; each maps to one diagnostic counter,
 * except REINIT (boot / fresh MIDI connect), which is a silent state clear —
 * it is not a host misbehavior, so it does not increment any of the three
 * named counters. */
typedef enum {
	LED_RELEASE_EXPLICIT,    /* host sent CC90 */
	LED_RELEASE_TIMEOUT,     /* no commit/heartbeat within LED_LEASE_TIMEOUT_MS */
	LED_RELEASE_DISCONNECT,  /* USB MIDI disconnect / interface-not-ready / suspend */
	LED_RELEASE_REINIT,      /* boot, or a fresh MIDI (re)connection */
} led_release_reason_t;

typedef struct {
	/* Staging: the host's full 8-value mental model. A stage() call only
	 * ever touches these fields, never `active` — "staging must not
	 * change visible outputs". */
	uint8_t  staged[LED_PHYSICAL_COUNT];
	uint16_t staged_mask; /* bit i set once index i has been staged since the last reset/release */

	/* Committed/active: exactly what the renderer shows while owned. */
	uint8_t  active[LED_PHYSICAL_COUNT];
	bool     owned;       /* true once a valid complete-or-later commit has landed */
	bool     had_commit;  /* at least one commit ever accepted since the last reset/release */
	uint8_t  last_seq;    /* valid iff had_commit */
	uint32_t lease_deadline_ms; /* valid iff owned */

	/* Diagnostics (cumulative; only led_frame_reset() zeroes them). */
	uint32_t valid_commits;
	uint32_t rejected_commits;
	uint32_t duplicate_commits;
	uint32_t lease_timeouts;
	uint32_t explicit_releases;
	uint32_t disconnect_releases;
} led_frame_state_t;

/* Full cold-boot reset: clears staging, active frame, ownership AND every
 * diagnostic counter. Call exactly once, at firmware boot. */
void led_frame_reset(led_frame_state_t *s);

/* Stage one physical index (0..7) with a level (clamped to 0..127). Never
 * touches `active`. Invalid index is ignored. */
void led_frame_stage(led_frame_state_t *s, uint8_t index, uint8_t level);

/* True once every one of the 8 indices has been staged since the last
 * reset/release. */
bool led_frame_all_staged(const led_frame_state_t *s);

/* Attempt to commit the current staged[] to active[] under sequence `seq`.
 * `now_ms` arms/refreshes the lease on acceptance. */
led_commit_result_t led_frame_commit(led_frame_state_t *s, uint8_t seq, uint32_t now_ms);

/* Refresh the lease if (and only if) ownership is already active. Never
 * creates ownership by itself. */
led_heartbeat_result_t led_frame_heartbeat(led_frame_state_t *s, uint8_t seq, uint32_t now_ms);

/* If owned and the lease has expired as of now_ms, release(TIMEOUT) and
 * return true. Otherwise a no-op returning false. Call once per main-loop
 * iteration (or on any read of ownership state) so a stale frame can never
 * outlive the lease. */
bool led_frame_check_lease_timeout(led_frame_state_t *s, uint32_t now_ms);

/* Clear ownership, clear staging completeness, and turn the active frame
 * off (all zero) — "a stale frame must never remain illuminated after the
 * website disappears". Bumps the diagnostic counter matching `reason`
 * (REINIT bumps none). A fresh complete 8-channel frame is required before
 * ownership can resume, exactly as after boot. */
void led_frame_release(led_frame_state_t *s, led_release_reason_t reason, uint32_t now_ms);

/* seq_a "newer than" seq_b under modulo-128 wraparound, using the standard
 * half-window (64) rule: forward distance in [1, 63] is newer, [64, 127] is
 * treated as older/stale, 0 is equal (duplicate). Exposed for the host
 * tests; led_frame_commit() uses this internally. */
int led_seq_compare(uint8_t seq_a, uint8_t seq_b);

/*
 * Render-source precedence (docs/stem-tape-led-feedback-v1.md "Safety
 * precedence"): does a safety pattern (DFU escape, fatal/reset, shutdown,
 * power-off countdown, boot signature) outrank a currently-owned host frame
 * this tick? A pure decision only — main.c calls this exact function every
 * main-loop iteration and is responsible for actually rendering the chosen
 * source, so what is tested here is what runs. `safety_active` covers every
 * safety state that can be concurrent with the main loop (in practice only
 * the power-off countdown; every other safety state is a blocking call that
 * cannot coexist with a render decision in the first place).
 */
typedef enum {
	LED_RENDER_SOURCE_PATTERN, /* local safety/boot/idle pattern wins */
	LED_RENDER_SOURCE_HOST,    /* leased host frame wins */
} led_render_source_t;

led_render_source_t led_render_select(bool safety_active, bool host_owned);

#endif /* STEMTAPE_LED_FRAME_H_ */
