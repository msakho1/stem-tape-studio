/*
 * st_gesture.h — Stem Tape standalone player: physical control scanner and
 * gesture grammar (task sections 3-7).
 *
 * PURE: no Zephyr, no ADC/GPIO access, no audio. main.c (Zephyr-only) reads
 * raw ladder codes / GPIO edges and calls st_gesture_process_event() with
 * plain (control, pressed, now_ms) triples; this module owns every
 * debounce/chord/hold/latch decision and emits zero or more semantic
 * commands. This is what makes "idle input produces zero semantic actions"
 * and "chord atomicity" host-testable without hardware.
 *
 * Scope note: this module implements every gesture EXPLICITLY specified in
 * the task (global loop momentary/latch/release, STEM/GLOBAL FX scope
 * open/close, FX Track momentary/latch/unlatch, the full scrub latch
 * grammar, fader pickup/crossing, FUNCTION+Volume context arbitration) with
 * full rigor.
 *
 * Track mute/solo/audition/link/song-selection is now driven from the real
 * rows in docs/firmware-contract-v1.json's tape layer, not a placeholder
 * subset:
 *   - bare Track tap                -> mute/unmute ("stop the take")
 *                                       [contract: track.tap]
 *   - PLAY + Track, < 700ms overlap -> solo toggle
 *   - PLAY + Track, >= 700ms overlap -> link toggle
 *                                       [contract: stem.solo / stem.link,
 *                                        stemSoloLinkThresholdMs=700]
 *   - FUNCTION + Track (first)      -> jump to that track's song bank
 *   - FUNCTION + Track (repeat, same
 *     track, FUNCTION still held)   -> next song within that bank
 *                                       [contract: track.bank, "banks: jump
 *                                        · tap again = next song"]
 * Precedence note (explicit task text ranks above the JSON contract): the
 * task's own text says "FUNCTION + Volume selects the previous/next active
 * stem" — that is kept as FUNCTION+Volume (see fire_single_volume_action())
 * even though the JSON contract's stem.select rows use PLAY+Volume instead;
 * PLAY+Volume is intentionally left unmapped.
 *
 * Audition (holding a bare Track on a currently-muted stem to preview it
 * unmuted while held, snapping back to muted on release) has NO row in
 * docs/firmware-contract-v1.json under that name — the only "audition" hit
 * is an unrelated Heads-mode "lane.audition" entry. This is therefore an
 * INFERRED behavior, not a copied spec: a short tap (<=
 * ST_GESTURE_BARE_TAP_MAX_MS) on a muted stem commits the unmute; a longer
 * hold previews it without committing. Flagged here and in the firmware
 * report, not silently presented as contract-verified.
 */

#ifndef STEMTAPE_PLAYER_GESTURE_H_
#define STEMTAPE_PLAYER_GESTURE_H_

#include <stdbool.h>
#include <stdint.h>

/* ---- timing constants ----
 * Reused verbatim from docs/FIRMWARE_CONTRACT_V1.md section 4 where that
 * contract already defines the exact boundary; new only where the task's
 * grammar (scrub latch, FX scope) has no prior constant. */
#define ST_GESTURE_PLAY_TAP_HOLD_MS         450u  /* [contract v1] playTapHoldMs */
#define ST_GESTURE_SOLO_LINK_THRESHOLD_MS   700u  /* [contract v1] stemSoloLinkThresholdMs */
#define ST_GESTURE_VOLUME_CHORD_WINDOW_MS   120u  /* [contract v1] fxOverlaySecondPressMs */
#define ST_GESTURE_BARE_TAP_MAX_MS          250u  /* firmware policy: FUNCTION press+release
						     * duration still counted as "a tap" for the
						     * scrub-unlatch / FX-track-repeat gestures */
#define ST_GESTURE_STARTUP_SETTLE_MS        150u  /* baseline capture window before any
						     * gesture is accepted -- "capture a stable
						     * startup baseline before accepting gestures" */
#define ST_GESTURE_FADER_DEADBAND           8u    /* raw 12-bit counts, same constant already
						     * proven in firmware/stemtape's FADER_DEADBAND
						     * and the pinned looper's own fader handling */

typedef enum {
	ST_CTRL_PLAY = 0,
	ST_CTRL_FUNCTION,
	ST_CTRL_TRACK1, ST_CTRL_TRACK2, ST_CTRL_TRACK3, ST_CTRL_TRACK4,
	ST_CTRL_VOL_MINUS, ST_CTRL_VOL_PLUS,
	ST_CTRL_ROCKER_FWD, ST_CTRL_ROCKER_RWD,
	ST_CTRL_COUNT,
} st_control_id_t;

typedef enum {
	ST_CMD_NONE = 0,

	/* transport */
	ST_CMD_PLAY_PAUSE_TOGGLE,
	ST_CMD_POWER_OFF_REQUEST,   /* long FUNCTION hold -- see st_gesture_process_tick() */

	/* mixer */
	ST_CMD_FADER_LEVEL,             /* .stem, .value_q8 */
	ST_CMD_MASTER_VOLUME_STEP,      /* .value_q8 = +1 or -1 (step direction) */
	ST_CMD_TRACK_MUTE_TOGGLE,       /* .stem : bare Track tap */
	ST_CMD_TRACK_SOLO_TOGGLE,       /* .stem : PLAY+Track, overlap < 700ms */
	ST_CMD_TRACK_LINK_TOGGLE,       /* .stem : PLAY+Track, overlap >= 700ms */
	ST_CMD_TRACK_AUDITION_START,    /* .stem : bare Track held on a muted stem --
					  * temporarily unmute for monitoring (INFERRED, see
					  * st_gesture.h header note) */
	ST_CMD_TRACK_AUDITION_END,      /* .stem : audition released, reverts to muted */
	ST_CMD_STEM_SELECT_PREV,
	ST_CMD_STEM_SELECT_NEXT,
	ST_CMD_SONG_BANK_JUMP,          /* .stem = bank index : first FUNCTION+TrackN this
					  * FUNCTION hold */
	ST_CMD_SONG_NEXT_IN_BANK,       /* .stem = bank index : repeat FUNCTION+TrackN (same N)
					  * while FUNCTION is still held */

	/* global loop (task section 5) */
	ST_CMD_LOOP_MOMENTARY_START,
	ST_CMD_LOOP_MOMENTARY_END,   /* PLAY released without ever latching */
	ST_CMD_LOOP_LATCH,
	ST_CMD_LOOP_EXIT,
	ST_CMD_LOOP_DIVISION_PREV,
	ST_CMD_LOOP_DIVISION_NEXT,

	/* FX (task section 6) */
	ST_CMD_FX_SCOPE_OPEN_STEM,
	ST_CMD_FX_SCOPE_OPEN_GLOBAL,
	ST_CMD_FX_SCOPE_CLOSE,
	ST_CMD_FX_STEM_CYCLE_PREV,
	ST_CMD_FX_STEM_CYCLE_NEXT,
	ST_CMD_FX_TRACK_MOMENTARY_START, /* .stem = bank index, see st_fx_catalog.h */
	ST_CMD_FX_TRACK_MOMENTARY_END,
	ST_CMD_FX_TRACK_LATCH,
	ST_CMD_FX_TRACK_UNLATCH,
	ST_CMD_FX_CLEAR_ALL_LATCHES,

	/* scrub (task section 7) */
	ST_CMD_SCRUB_MOMENTARY_START,  /* .value_q8: 1 = forward, -1(as 0xFF) = reverse via .direction */
	ST_CMD_SCRUB_LATCH_ARM,
	ST_CMD_SCRUB_RELEASE,          /* momentary scrub released, not latched: ramp to 1.0x */
	ST_CMD_SCRUB_UNLATCH,          /* latched scrub explicitly unlatched: ramp to 1.0x */
	ST_CMD_SCRUB_SPEED_SELECT,     /* .stem reused as speed index 0..3 */
} st_cmd_id_t;

typedef struct {
	st_cmd_id_t id;
	uint8_t     stem;      /* meaning depends on id: stem index / bank index / speed index */
	int8_t      direction; /* +1 / -1, meaning depends on id (scrub direction, step direction) */
	uint8_t     value_q8;  /* meaning depends on id (fader level, master volume magnitude) */
} st_cmd_t;

#define ST_GESTURE_MAX_CMDS_PER_EVENT 4u

typedef struct {
	st_cmd_t cmds[ST_GESTURE_MAX_CMDS_PER_EVENT];
	uint8_t  count;
} st_cmd_batch_t;

/* ---- internal per-control edge tracking ---- */
typedef struct {
	bool     down;
	uint32_t down_since_ms;   /* valid iff down */
	bool     joined;          /* this press was consumed as part of a chord/latch gesture
				    * (e.g. FUNCTION joined with the rocker) -- used to
				    * suppress a "bare tap" interpretation on release */
} st_control_state_t;

typedef enum {
	ST_FX_SCOPE_NONE = 0,
	ST_FX_SCOPE_STEM,
	ST_FX_SCOPE_GLOBAL,
} st_fx_scope_t;

typedef struct {
	/* startup baseline */
	bool     settled;
	uint32_t boot_ms;

	st_control_state_t ctrl[ST_CTRL_COUNT];

	/* PLAY / global loop */
	bool     playing;
	bool     loop_momentary_active;
	bool     loop_latched;

	/* FUNCTION+Volume chord detection */
	uint32_t vol_minus_down_ms; bool vol_minus_down;
	uint32_t vol_plus_down_ms;  bool vol_plus_down;
	bool     function_was_down_before_vol_chord; /* GLOBAL vs STEM scope discriminator */

	/* FX */
	st_fx_scope_t fx_scope;
	uint8_t       fx_stem_index;      /* active-stem cursor while STEM scope is open */
	uint8_t       fx_track_holding;   /* 0xFF = none, else bank index currently held */
	bool          fx_track_latched[4];

	/* scrub */
	bool    scrub_active;      /* momentary or latched */
	bool    scrub_latched;
	int8_t  scrub_direction;   /* +1 fwd, -1 rev, 0 = inactive */
	bool    scrub_latch_armed_this_hold;
	uint8_t scrub_speed_index; /* 0..3, persists (caller is responsible for saving it) */

	/* mixer */
	uint8_t active_stem;
	uint16_t fader_raw_last[4]; /* 0xFFFF = "never sampled" (pickup pending) */
	bool     fader_picked_up[4];

	/* track mute/solo/link/audition/song-select state, owned here so the
	 * tap-vs-hold and audition-vs-toggle decisions can see current state
	 * (mirrors playing/loop_latched/scrub_latched already being owned
	 * locally) -- the audio engine mirrors these via the emitted commands. */
	uint8_t  stem_mute_mask;
	uint8_t  stem_solo_mask;
	uint8_t  stem_link_mask;
	bool     track_audition_active[4];
	bool     track_play_chord[4];   /* true if PLAY was down when this Track was pressed */
	uint8_t  song_bank_last_tapped; /* 0xFF = none yet this FUNCTION hold */

	/* long FUNCTION hold -> power off, tracked by the caller via
	 * st_gesture_process_tick() since it depends on wall-clock elapsed
	 * time, not just edges. */
	uint32_t function_down_since_ms;
} st_gesture_state_t;

/* Cold-boot reset. `now_ms` seeds the startup-settle window. */
void st_gesture_reset(st_gesture_state_t *s, uint32_t now_ms);

/* True once the startup baseline is captured and gestures are accepted.
 * Before this, every edge is absorbed silently (no commands emitted) --
 * "do not treat boot, connection, resync, or initial fader snapshots as
 * user input". */
bool st_gesture_is_settled(const st_gesture_state_t *s, uint32_t now_ms);

/* Feed one physical edge. `pressed` = new logical state (debounced by the
 * caller's ladder-band + hysteresis scan, same measured-band methodology
 * already proven in firmware/stemtape's decode_bands()/DEBOUNCE_PASSES --
 * this module receives clean edges, it does not itself debounce raw ADC
 * noise). Emits zero or more commands into `out`. */
void st_gesture_process_edge(st_gesture_state_t *s, st_control_id_t ctrl, bool pressed,
			      uint32_t now_ms, st_cmd_batch_t *out);

/*
 * Feed one fader's raw ADC reading (0..4095, same 12-bit convention as the
 * rest of this codebase). Applies ST_GESTURE_FADER_DEADBAND jitter
 * suppression and PICKUP: after a song/slot load (call
 * st_gesture_arm_fader_pickup()), the first N readings are absorbed
 * silently until the physical fader crosses the persisted level, at which
 * point it "picks up" and starts emitting ST_CMD_FADER_LEVEL again --
 * never an instant jump to the physical position. Emits at most one
 * command.
 */
void st_gesture_process_fader(st_gesture_state_t *s, uint8_t stem, uint16_t raw12,
			       uint16_t persisted_raw12, uint32_t now_ms, st_cmd_t *out);

/* Call after loading a song/slot (or on boot) so the next physical fader
 * motion requires crossing the persisted level before it takes effect,
 * instead of snapping to wherever the physical fader currently sits. */
void st_gesture_arm_fader_pickup(st_gesture_state_t *s, uint8_t stem);

/*
 * Call once per scan tick (not just on edges) so time-based transitions
 * fire even with no new edge this tick: the long-FUNCTION power-off hold,
 * and the "bare FUNCTION tap" scrub-unlatch window closing. Emits zero or
 * more commands.
 */
void st_gesture_process_tick(st_gesture_state_t *s, uint32_t now_ms, st_cmd_batch_t *out);

#endif /* STEMTAPE_PLAYER_GESTURE_H_ */
