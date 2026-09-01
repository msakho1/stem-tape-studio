#!/usr/bin/env python3
"""Stem Tape Player -- stored four-stem playback wiring check (fail-closed).

Phase 2 slice 3B.1: proves the real production audio-data-path call
sequence -- continuous, multi-sector streaming via st_stem_stream.h's pure
state machine, handed off between threads through st_stem_bufmbox.h's
atomic mailbox -- is genuinely present in main.c's own source, not merely
that the callee symbols exist somewhere in the link (the runtime symbol-
presence gate's own CI step already proves THAT separately). Source-level
call-site proof, not link-level: reuses index_functions() from
stemtape_player_safety_gate.py (the SAME brace-depth enclosing-function
parser this repo's own write-safety gate already depends on and self-
tests -- see stemtape_player_safety_gate_parser_selftest.py) rather than
reimplementing a second C-source scanner.

Checks (every one a REAL call expression -- `symbol(`-- found, via
index_functions()'s own brace-depth tracking, textually inside the named
enclosing function's own body, skipping comment lines the same way
find_call_sites() in stemtape_player_safety_gate.py already does):

  1. The audio path is TWO functions, and both are checked in the
     function that genuinely makes each call.

     looper_audio_block() (the real-time mixer, called every I2S block
     from audio_thread()) is the mailbox CONSUMER and the RUN DECIDER: it
     calls st_stream_required_sector()/st_stream_sector_ready()/
     st_stream_advance_frames() (the pure streaming state machine,
     exclusively audio-thread-owned) plus st_stem_mbox_try_acquire()/
     st_stem_mbox_set_requested_sector() (the mailbox's consumer-side
     API) plus st_stem_mix_prepare() (mute/solo/gain ceiling, resolved
     once per block) -- and drives stem_render_run().

     stem_render_run() is the tight -O2 renderer: it calls
     st_pl_decode_frame_shared() (the real v1.2 per-frame decoder,
     RAM-only, no I/O) and st_stem_mix_frame_prepared() (the real
     4-stem-to-stereo mixdown).

     The PLURAL st_stream_advance_frames() is required on purpose.
     Advancing one frame at a time is exactly the per-frame cost this
     structure removed -- along with the per-frame sector division,
     residency test and barriered mailbox atomic -- so requiring the run
     form is what stops a silent regression back to it. The two forms are
     proven equivalent over a whole real song (identical song_frame,
     state, underrun count and audio hash) in tests/test_stem_stream.c.

     Together this is the "stored four-stem playback path actually
     references/uses st_stem_stream/st_stem_bufmbox/st_stem_mix"
     requirement: not incidental symbol presence, real call sites inside
     the real real-time audio functions.

  2. streamer_thread() (the one thread that ever touches flash) -- the
     mailbox PRODUCER -- calls st_stream_init() (seeding the state
     machine from the real selected song's own STIX geometry, once, at
     boot) and stem_read_groups() (validating EVERY group
     read, not just the first -- read-only geometry access, safe from
     the producer thread) plus st_stem_mbox_init()/st_stem_mbox_
     producer_next_run()/st_stem_mbox_publish_ready() (the atomic
     ring's producer-side API) -- proving both the boot-time first
     sector AND the continuous per-pass prefetch that streams the rest
     of the song go through the real state machine and the real atomic
     handoff, never a shared struct touched directly by both threads.

  3. audio_thread() (the real I2S TX loop) calls looper_audio_block()
     (the decoder+mixer, check 1 above) and i2s_write() -- proving the
     final link Phase 2's own "full playback gate" slice requires: the
     mixer's real stereo output feeds the SAME I2S write call site the
     golden classic bus already used, unconditionally, every block --
     not a separate/parallel output path. This is the "prove the real
     streamer, decoder, mixer AND I2S caller chain is linked" proof, at
     the source level (main.c is not itself host-testable -- see tests/
     test_stem_playback_gate.c's own doc comment for the complementary,
     REAL two-thread algorithmic proof over the real fixture).

  4. Phase 3 control-matrix (fader + mute + solo, momentary hold
     corrected): looper_audio_block() builds its st_stem_mix_channel_t
     array from the SAME control surface the classic engine's own PASS
     A/B already read (trk[s].vol_q8, trk[s].muted, trk[s].solo -- see
     main.c's own PASS C comment for why that cross-thread read is
     safe), not from a hardcoded placeholder. This is a substring check,
     not a call-site check (an array-field read is not a call
     expression), so it is REQUIRED_SUBSTRINGS below, checked the same
     fail-closed way. Also proves main() genuinely CALLS
     st_track_hold_tick() (REQUIRED_CALLS -- the pure, host-tested
     momentary hold-timing state machine that actually drives trk[].solo
     now, not a hardcoded placeholder or the earlier, corrected release-
     time toggle) AND that main()'s own release-episode handler reads
     track_hold[ti].solo_active (REQUIRED_SUBSTRINGS) to decide whether
     to suppress its own tap-to-mute action -- both ends of the
     corrected wire proven present in source, not just one.

  5. LED slice: led_service() is the SINGLE semantic owner of all eight
     LEDs -- it gathers live state and hands it to one pure decision,
     and it must not decide anything itself. This gate requires it to
     call st_led_batt_classify(), st_led_mvp_decide() and
     led_apply_frame() (REQUIRED_CALLS), and to read
     atomic_get(&g_stem_song_selected) and trk[i].solo
     (REQUIRED_SUBSTRINGS) -- proving the live transport/mixer state
     really reaches the pure decision, and that the decision's output
     really reaches the physical renderer. The forbidden lists below are
     the other half of that proof: no second renderer, no competing
     legacy mask, and no persistent per-track mute concept (Stem Tape's
     Track button is an immediate momentary solo, never a mute).

  6. Beat/chase from real STIX timing: streamer_thread()'s boot code
     genuinely reads lib.active.bpm_q8/downbeat_frame (the selected STIX
     record's own authoritative song-level timing -- and under v1.2 the
     ONLY source of it, since a group header carries identity rather
     than tempo) before calling the real st_beat_timing_init()
     (REQUIRED_CALLS); audio_block_epilogue() genuinely calls
     atomic_set(&g_stem_song_frame_pub, ...) once per block
     (REQUIRED_SUBSTRINGS -- an atomic-write call whose OWN argument is
     what proves it is the right one, not just any atomic_set); and
     led_service() calls st_beat_pulse() (REQUIRED_CALLS) on BOTH
     &g_stem_beat_timing and atomic_get(&g_stem_song_frame_pub)
     (REQUIRED_SUBSTRINGS). Requiring both arguments by name is what
     stops a second, free-running LED clock from reappearing: the pulse
     can only be derived from the same song position the audio path
     publishes.

     WHAT USED TO BE HERE, and the correction that reversed it. This gate
     once required led_service() to call st_beat_phase_on_beat()/
     st_beat_led_decide(). That pair derived ONE boolean from the STIX
     tempo and handed the SAME value to all four Track LEDs, so they
     flashed uniformly, carrying no bar position and no dynamics. They
     are DELETED from src/st_beat_phase.c and still forbidden below.

     The gate then went the other way and FORBADE per-stem metering
     (src/st_stem_meter.c), on the reasoning that "a level meter dimmed a
     stem that was plainly audible during a quiet passage". That is
     reversed, and stated plainly because it was the wrong call: dimming
     during a quiet passage is the display WORKING. The Track row's job
     is to show what each part of the arrangement is doing, and a row
     that reads the same whether a stem is resting or driving the song
     shows nothing. The intervening design -- a shared beat envelope,
     merely SCALED per stem, dark between pulses -- was four tempo
     indicators: it told the player what the clock was doing, which they
     could already hear, instead of what each stem was doing, which they
     could not see any other way.

     The Track row now carries each stem's own enveloped level, with no
     beat gate and no chase accent, and st_stem_meter_update()/
     st_stem_meter_brightness() are REQUIRED in led_service() below.
     st_beat_timing_init() is still called and still required -- the
     tempo is still parsed, and S4 still shows it. What changed is only
     what the Track row consumes.

Fails closed: main.c missing, either function's body not found, or any
required call site/substring absent.

Usage: stemtape_player_stem_playback_wiring_check.py <main.c> <out-report.md>
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from stemtape_player_safety_gate import function_body_bounds, index_functions  # noqa: E402

REQUIRED_CALLS = {
    # THE AUDIO PATH IS THREE FUNCTIONS, and each call is required in the one
    # that genuinely makes it.
    #
    #   looper_audio_block()  -- DISPATCHES. Its first act is the Stem Tape
    #       decision; when a stem song is playing it renders through
    #       stem_audio_block() and RETURNS, so none of the inherited classic
    #       recorder/transport/PASS A/PASS B work below it executes at all.
    #   stem_audio_block()    -- the fast path: channel strip, mailbox, stream
    #       bookkeeping, run dispatch.
    #   stem_render_run()     -- the tight -O2 per-frame decode+mix loop.
    #
    # Splitting them this way is the fix for the physical playback failure, so
    # the split itself is what these checks pin. See also the BYPASS section
    # below, which proves the dispatch really is a bypass and not another
    # conditional downstream of the expensive passes.
    "looper_audio_block": [
        "stem_audio_block",
        "master_vol_ramp",
        "audio_block_epilogue",
        "st_stream_play",
        "st_stream_stop",
    ],
    "stem_audio_block": [
        # THE LOOP'S WRAP AND EXIT BOTH GO THROUGH THE REAL SEEK. Requiring
        # the call here is what stops either one regressing to a bare
        # song_frame assignment that would leave residency claiming a sector
        # this frame does not live in -- the stale-data failure.
        "st_stream_seek",
        "st_stem_mix_prepare",
        "stem_render_run",
        "st_stream_required_sector",
        "st_stream_sector_ready",
        # The RUN form, not the per-frame form: advancing frame-by-frame is
        # exactly the cost this structure removed, so requiring the plural
        # name is what stops a regression back to it. The two are proven
        # equivalent over a whole real song in tests/test_stem_stream.c.
        "st_stream_advance_frames",
        "st_stem_mbox_try_acquire",
        "st_stem_mbox_set_requested_sector",
    ],
    "stem_render_run": [
        # v1.2: the decode is now four per-stem GROUPS rather than one
        # interleaved sector, which is the change that makes a stem's read
        # address independent of the others'. The _shared form is the one the
        # hot loop uses while all four heads are together; per-track reverse
        # is what starts passing different indices, through the array form.
        "st_pl_decode_frame_shared",
        # THE INLINE FORM, and requiring it by name is deliberate.
        #
        # st50 moved the mixdown arithmetic into st_stem_mix.h as
        # st_stem_mix_frame_prepared_inline(); the out-of-line
        # st_stem_mix_frame_prepared() now calls that, so there is still
        # exactly one implementation and the host tests and the FX gate
        # still exercise the code this loop runs. What changed is which
        # entry point the 48 kHz path uses: a pointer to an
        # st11_audio_frame_t crossing a translation unit obliged the
        # compiler to spill eight int32_t to the stack and the callee to
        # load them back, 48,000 times a second, computing nothing.
        #
        # The name checked here has to be the one the render loop actually
        # calls or this gate stops proving anything -- and it must be the
        # _inline suffix specifically, because the bare name is a prefix of
        # it: accepting the short form would let a regression back to the
        # out-of-line call pass unnoticed.
        "st_stem_mix_frame_prepared_inline",
    ],
    "streamer_thread": [
        "st_stream_init",
        # v1.2 REPLACES THE SECTOR-HEADER CHECK WITH A GROUP-HEADER ONE, and
        # it is a stronger check, not a weaker one. A v1.1 STSC header could
        # say "I am sector 7"; it could not say which stem it was, because a
        # sector held all four. A group header names the stem AND the span, so
        # stem_read_groups() rejects a read that landed in another stem's
        # region -- the failure mode song-planar newly makes possible, caught
        # by construction rather than heard.
        "stem_read_groups",
        "stem_prime_group0",
        "st_stem_mbox_init",
        # The BATCHED producer, not the single fill: four per-stem rings cost
        # four fills per span, and requiring the run form is what stops a
        # regression to one-group-per-read (5147 us against a 7083 us span).
        "st_stem_mbox_producer_next_run",
        "st_stem_mbox_publish_ready",
        "st_beat_timing_init",
    ],
    "audio_thread": [
        "looper_audio_block",
        "i2s_write",
    ],
    "main": [
        "st_track_hold_tick",
        # THE ONE DISPATCHER. Every Stem Tape control decision -- the ladder
        # classification, the Track mask, the PLAY gesture, the loop grammar
        # -- runs through st_ctl_service(), called once per control pass.
        # Requiring the CALL is what stops any of it regressing to an inline
        # re-implementation, or to decode_tracks()' single identity, which
        # cannot express "two pressed" at all.
        "st_ctl_service",
        # ...and its result must actually be applied to the device.
        "stem_ctl_apply",
        # COLD BOOT. The one-bar default and every gesture edge are reset
        # exactly once, at startup.
        "st_ctl_reset",
    ],
    "led_service": [
        # THE SINGLE SEMANTIC LED OWNER. led_service() must gather live state
        # and hand it to the pure decision -- it must not decide anything
        # itself.
        "st_led_batt_classify",
        "st_led_mvp_decide",
        "led_apply_frame",
        # BEAT/CHASE FROM STIX TIMING. led_service() must derive the pulse
        # from the real tempo snapshot and the live song position -- not
        # from a clock of its own. This still drives S4; it no longer
        # reaches the Track row.
        "st_beat_pulse",
        # THE AUDIO-REACTIVE TRACK ROW. Both halves are required, and
        # requiring them HERE is what makes "the lights follow the audio" a
        # property of the source rather than a promise: the envelope must be
        # advanced from live per-stem peaks, and its brightness must be what
        # reaches st_led_inputs_t. A build that linked the meter but never
        # called it would show a beat-driven or dead row and still pass a
        # symbol-presence check.
        "st_stem_meter_update",
        "st_stem_meter_brightness",
    ],
}

# ---------------------------------------------------------------------------
# BYPASS PROOF (the reason this whole restructure exists).
#
# Requirement 1 of the physical-playback fix: when a validated Stem Tape song
# is selected, the inherited classic-loop transport, resampling, recording,
# classic mixing and classic play-ring processing must be BYPASSED -- not run
# first and then ignored. Two independent, fail-closed checks:
#
#   A. stem_audio_block() may not NAME any classic-engine symbol. It cannot
#      accidentally depend on classic work if it cannot refer to it.
#   B. looper_audio_block() must reach its stem `return;` BEFORE the first
#      classic-engine statement in its own body. This is what makes the
#      dispatch a bypass: a version that ran PASS A/PASS B first and then
#      branched would fail here even though every call-site check above still
#      passed.
CLASSIC_SYMBOLS = ["mix32", "posb", "fracb", "vol_s", "pring", "soft_limit",
                   "g_loop_active", "g_pphase", "g_consume_pos", "g_cur_speed_q16"]
# First classic statement markers, searched in looper_audio_block()'s body.
CLASSIC_MARKERS = ["mix32[", "posb[", "fracb[", "vol_s[", "g_rec_track", "g_pphase"]

# ---------------------------------------------------------------------------
# LED OWNERSHIP PROOF.
#
# The whole point of the LED repair is that exactly ONE thing decides what the
# eight LEDs show. Two fail-closed checks make that structural rather than a
# convention someone has to remember:
#
#   A. These names must not exist ANYWHERE in main.c -- not as a definition,
#      not as a call. show_song_leds() painted the side row as a 16-song
#      bank/position display, which on a one-song device blinked a side LED
#      forever to announce "song 1 of 16"; that is the physical symptom this
#      work exists to remove. Requiring its ABSENCE, rather than requiring
#      that the Stem Tape path avoids calling it, is what makes "it cannot
#      control the side LEDs in Stem Tape mode" a property of the source
#      instead of a promise.
#
#   B. led_service() must not write the physical LED masks directly. Every
#      pin write belongs to led_apply_frame(), which renders a frame the
#      pure decision produced. If led_service() could still call
#      track_led_on()/led_on()/etc. it would be a second owner again, which
#      is exactly the state this replaced.
LED_FORBIDDEN_ANYWHERE = [
    "show_song_leds",     # the inherited 16-song bank/position side display
    "g_trk_level_active",       # the meter's renderer gate
    "led_apply_mode",           # the superseded three-state applier
    # The retired tempo boolean and the on/off/ghost track decision it fed.
    # Deleted outright from src/st_beat_phase.c (see this file's own docstring,
    # check 6): one boolean handed to all four Track LEDs makes them flash
    # uniformly with no bar position, which is the exact display this firmware
    # was corrected away from. st_beat_pulse() is the replacement. Naming them
    # here means re-wiring either one fails with this explanation rather than
    # an unexplained link error.
    "st_beat_phase_on_beat",
    "st_beat_led_decide",
]

# ======================================================================
# THE LOOP MAY NOT MOVE THE PLAYHEAD EXCEPT AT A BOUNDARY.
# ======================================================================
# Engaging or releasing the loop changes the transport's RULES, never its
# POSITION. Only the playhead crossing loopEnd may move it.
#
# The firmware used to seek BACK to the captured frame on engage --
# loop_start is captured at PLAY-DOWN, so that replayed the whole hold --
# and forward to loop_end on release. On a vocal reading "for me no",
# engaging during "me" gave "for me - me no", with the syllable audibly
# restarting. Reported from hardware.
#
# Each name below is a load-bearing part of one of those two seeks. Naming
# them here means reintroducing either one fails with THIS explanation
# rather than as a silent behaviour change that only a listener would
# catch. Comments may still discuss them; code may not use them.
LOOP_FORBIDDEN_ANYWHERE = [
    "ST_SEAM_JUMP_ENTER",     # the entry seek's jump kind
    "ST_SEAM_JUMP_EXIT",      # the release seek's jump kind
    "g_stem_loop_enter_fr",   # the frame the entry seek landed on
    "g_stem_loop_resume_fr",  # the frame the release seek landed on
]

# led_service() must not read trk[].muted at all: Stem Tape has no persistent
# mute, and a display that consulted one would be showing state no gesture in
# this firmware can produce.
LED_FORBIDDEN_IN_LED_SERVICE_EXTRA = ["trk[i].muted"]
LED_FORBIDDEN_IN_LED_SERVICE = [
    "track_led_on(", "track_led_off(", "track_led_ghost(",
    "led_on(", "led_off(", "track_all_off(",
    "g_led_level[", "g_led_p0_on", "g_led_p1_on",
] + LED_FORBIDDEN_IN_LED_SERVICE_EXTRA

# Substring checks (see REQUIRED_CALLS's doc comment, check 4): these are
# array-field reads/assignments, not call expressions, so they cannot be
# found by calls_in_function()'s `name(` regex -- a plain per-line substring
# search within the function's own body (skipping comment-only lines, same
# as calls_in_function()) is enough, since each string here is specific
# enough not to appear incidentally in unrelated code or in a real (non-
# comment) statement other than the one it is meant to prove.
REQUIRED_SUBSTRINGS = {
    "stem_audio_block": [
        # The window bounds the run, so the wrap lands on a frame boundary
        # instead of inside a rendered run. Both gesture edges are ONE-SHOTS
        # consumed with an atomic CAS, so one gesture is seen exactly once.
        "atomic_cas(&g_stem_loop_enter_req, 1, 0)",
        "atomic_cas(&g_stem_loop_exit_req, 1, 0)",
        # THE PERIODIC BOUNDARY. With no entry seek the playhead can begin
        # OUTSIDE the window -- loop_start is captured at PLAY-DOWN and the
        # hold can outlast a short division -- so the boundary approached is
        # the first loop_start + n*len above it. Without this a short loop
        # would silently never wrap, which is worse than a click because it
        # looks like nothing happened.
        "lp_end = lp_lo + ((pos - lp_lo) / len + 1u) * len;",
        "run > left_in_loop",
        "trk[s].vol_q8",
        "trk[s].muted",
        "trk[s].solo",
    ],
    "audio_block_epilogue": [
        "atomic_set(&g_stem_song_frame_pub",
    ],
    "main": [
        "track_hold[ti].solo_active",
        # ONE SAMPLE PER PASS, fed to the ONE classifier. The dispatcher must
        # be given the pass's own ladder reading, and the inherited decode
        # must be given that same reading -- never a second conversion.
        "ci.ladder_raw     = st_trk_raw",
        "int trk_raw = st_trk_raw",
        # THE INHERITED DECODE IS BLIND IN STEM MODE. With a stem song
        # selected it is handed TRK_NONE, so no classic gesture -- including
        # the 400 ms hold-to-restart -- can fire underneath the dispatcher.
        "if (stem_ctl) {",
        # ...and the restart is additionally gated by name.
        "if (!stem_ctl && committed == TRK_PLAY) {",
        # FUNCTION ARBITRATION HAPPENS FIRST. The dispatcher runs above the
        # FUNCTION branch, and a press it consumed never reaches that branch
        # -- which is what makes the loop latch reachable at all.
        "if (pwr_pressed() && !g_stem_ctl_out.function_consumed) {",
    ],
    "stem_ctl_apply": [
        # THE PUBLISHED MASK IS WHAT REACHES THE MIXER.
        "trk[k].solo = ((o->track_mask >> k) & 1u) ? 1u : 0u;",
        # THE EXIT IS A FLAG, NOT A SEEK. Clearing `active` is the entire
        # exit as far as the transport is concerned. The resume frame is
        # deliberately no longer published or consumed: a release may not
        # move the playhead, so there is nothing to seek to. Its absence is
        # asserted in FORBIDDEN_SUBSTRINGS below.
        "atomic_set(&g_stem_loop_active, 0);",
        # THE ENTRY IS A FLAG TOO -- the one-shot edge, with no frame.
        "atomic_set(&g_stem_loop_enter_req, 1);",
        # BOTH PINNED REGIONS ARE REQUESTED FROM THE ARM ONWARDS, so the
        # wrap back to loop_start lands on resident bytes from the first
        # lap onward.
        "g_stem_loop_pin_want[ST_LOOP_PIN_ENTRY]",
        "g_stem_loop_pin_want[ST_LOOP_PIN_EXIT]",
        # THE ONLY PLACE the transport toggles for a Stem Tape song.
        "if (o->play_tap) {",
    ],
    "led_service": [
        "atomic_get(&g_stem_song_selected)",
        # The LED row must build its mask FROM trk[].solo -- the value that
        # actually reached the mixer -- so what is lit is what is heard.
        "in.solo_mask |= (uint8_t)(1u << i)",
        # The pulse's two inputs, by name: the STIX-derived tempo snapshot
        # and the authoritative song-frame mirror. Requiring BOTH is what
        # stops a second, free-running LED clock reappearing.
        "&g_stem_beat_timing",
        "atomic_get(&g_stem_song_frame_pub)",
        "trk[i].solo",
    ],
    "stem_render_run": [
        # Per-stem activity published from the SAME prepared gain the mixer
        # multiplies by -- one audibility rule, not two, so a stem that is
        # silenced in the mix can never scale a lit Track LED.
        "prep->gain_q8[sp] == 0",
    ],
    "streamer_thread": [
        "lib.active.bpm_q8",
        "lib.active.downbeat_frame",
        # NO hdr.bpm_q8 / hdr.downbeat_frame ANY MORE, and nothing weakened.
        # v1.1's 32-byte STSC header repeated the tempo fields and this gate
        # required the boot code to cross-check them -- but the check only ever
        # LOGGED a disagreement: the STIX record always won and the sector copy
        # was never acted on. A v1.2 group header is eight bytes of identity
        # (magic, stem, span) and carries no timing at all, so the single
        # authority is now also the only one. What replaced the check is
        # stronger and lives in REQUIRED_CALLS: stem_read_groups() validates
        # every group against the stem AND span it was asked for, which a
        # sector header could not do because a sector held all four stems.
    ],
}


def calls_in_function(lines: list[str], func_of_line: dict[int, str | None], func_name: str) -> set[str]:
    """Every bare-call symbol (`name(`, skipping comment-only lines) found
    textually inside `func_name`'s own body, using func_of_line's already-
    computed enclosing-function map to know which lines belong to it."""
    found: set[str] = set()
    for i, line in enumerate(lines, 1):
        if func_of_line.get(i) != func_name:
            continue
        stripped = line.strip()
        if stripped.startswith(("*", "//")):
            continue
        for m in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", line):
            found.add(m.group(1))
    return found


def substrings_in_function(lines: list[str], func_of_line: dict[int, str | None], func_name: str) -> str:
    """Every non-comment line of `func_name`'s own body, joined -- cheap
    enough at main.c's size, and simpler than tracking per-substring
    line hits when REQUIRED_SUBSTRINGS' own strings are already specific
    enough not to false-positive against unrelated code."""
    kept: list[str] = []
    for i, line in enumerate(lines, 1):
        if func_of_line.get(i) != func_name:
            continue
        stripped = line.strip()
        if stripped.startswith(("*", "//")):
            continue
        kept.append(line)
    return "\n".join(kept)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    main_c_path, out_path = sys.argv[1], sys.argv[2]

    try:
        text = open(main_c_path, errors="ignore").read()
    except OSError as e:
        print(f"FATAL: could not read {main_c_path}: {e}", file=sys.stderr)
        return 1
    lines = text.splitlines()

    func_of_line = index_functions(lines)
    defined_functions = set(v for v in func_of_line.values() if v is not None)

    report: list[str] = ["# Stem Tape Player -- stored four-stem playback wiring check", ""]
    fail = False

    for func_name, required_symbols in REQUIRED_CALLS.items():
        if func_name not in defined_functions:
            report.append(f"**MISSING** `{func_name}` is not defined in {main_c_path} at all")
            fail = True
            continue
        found = calls_in_function(lines, func_of_line, func_name)
        report.append(f"### `{func_name}()`")
        for sym in required_symbols:
            if sym in found:
                report.append(f"- present: real call site to `{sym}(` inside `{func_name}()`'s own body")
            else:
                report.append(f"- **MISSING**: no call site to `{sym}(` found inside `{func_name}()`'s own body")
                fail = True
        report.append("")

    for func_name, required_strings in REQUIRED_SUBSTRINGS.items():
        if func_name not in defined_functions:
            report.append(f"**MISSING** `{func_name}` is not defined in {main_c_path} at all")
            fail = True
            continue
        body_text = substrings_in_function(lines, func_of_line, func_name)
        report.append(f"### `{func_name}()` (substring checks)")
        for s in required_strings:
            if s in body_text:
                report.append(f"- present: `{s}` found inside `{func_name}()`'s own body")
            else:
                report.append(f"- **MISSING**: `{s}` not found inside `{func_name}()`'s own body")
                fail = True
        report.append("")

    # ---- BYPASS PROOF (see CLASSIC_SYMBOLS/CLASSIC_MARKERS above) ----
    report.append("### Stem Tape bypasses the classic engine (not merely ordered around it)")

    # A. the fast path cannot even name the classic engine
    fast_body = substrings_in_function(lines, func_of_line, "stem_audio_block")
    if not fast_body:
        report.append("- **MISSING**: stem_audio_block() not found -- the Stem Tape fast path "
                       "must be its own function")
        fail = True
    else:
        leaked = [n for n in CLASSIC_SYMBOLS if re.search(r"\b" + re.escape(n) + r"\b", fast_body)]
        if leaked:
            report.append("- **MISSING/BAD**: stem_audio_block() names classic-engine state: "
                           + ", ".join("`" + n + "`" for n in leaked))
            fail = True
        else:
            report.append("- present: stem_audio_block() names none of "
                           + ", ".join("`" + n + "`" for n in CLASSIC_SYMBOLS)
                           + " -- it cannot depend on classic work it cannot refer to")

    # B. the dispatch RETURNS before the first classic statement
    disp_lines = [i for i, l in enumerate(lines, 1)
                  if func_of_line.get(i) == "looper_audio_block"]
    if not disp_lines:
        report.append("- **MISSING**: looper_audio_block() body not found")
        fail = True
    else:
        ret_line = None
        first_classic = None
        for i in disp_lines:
            stripped = lines[i - 1].strip()
            if stripped.startswith(("*", "//", "/*")):
                continue
            if ret_line is None and stripped == "return;":
                ret_line = i
            if first_classic is None and any(m in stripped for m in CLASSIC_MARKERS):
                first_classic = i
        if ret_line is None:
            report.append("- **MISSING**: looper_audio_block() has no early `return;` -- the stem "
                           "dispatch must return, not fall through into the classic engine")
            fail = True
        elif first_classic is not None and ret_line > first_classic:
            report.append(f"- **MISSING/BAD**: classic-engine work at line {first_classic} runs "
                           f"BEFORE the stem dispatch returns at line {ret_line} -- that is an "
                           "ordering, not a bypass")
            fail = True
        else:
            where = f"line {first_classic}" if first_classic else "none present"
            report.append(f"- present: the stem dispatch returns at line {ret_line}, before the "
                           f"first classic-engine statement ({where}) -- classic transport, "
                           "recording, resampling, PASS A and PASS B never execute for a "
                           "stem-rendered block")
    report.append("")

    # ---- LED OWNERSHIP PROOF (see LED_FORBIDDEN_* above) ----
    report.append("### Exactly one semantic owner of the eight LEDs")

    # A. the legacy displays must not exist in the source at all
    code_lines = []
    for i, l in enumerate(lines, 1):
        stripped = l.strip()
        if stripped.startswith(("*", "//", "/*")):
            continue          # comments may DISCUSS them; code may not use them
        code_lines.append((i, l))
    for name in LED_FORBIDDEN_ANYWHERE:
        hits = [i for i, l in code_lines if re.search(r"\b" + re.escape(name) + r"\b", l)]
        if hits:
            report.append(f"- **MISSING/BAD**: `{name}` still present in main.c at line(s) "
                           + ", ".join(str(h) for h in hits[:5])
                           + " -- every superseded LED mechanism (the legacy song-bank display, "
                             "the ad-hoc peak meter, the retired tempo boolean and its on/off/"
                             "ghost decision) must be GONE, not merely unreferenced from the "
                             "Stem Tape path")
            fail = True
        else:
            report.append(f"- present: `{name}` does not exist anywhere in main.c's code -- it "
                           "cannot drive any LED in any state")

    report.append("")
    report.append("### The loop never moves the playhead except at a boundary")
    for name in LOOP_FORBIDDEN_ANYWHERE:
        hits = [i for i, l in code_lines if re.search(r"\b" + re.escape(name) + r"\b", l)]
        if hits:
            report.append(f"- **MISSING/BAD**: `{name}` is back in main.c at line(s) "
                           + ", ".join(str(h) for h in hits[:5])
                           + " -- an entry or release SEEK has returned. Engaging or releasing "
                             "the loop may change the transport's rules but never its position; "
                             "only crossing loopEnd may move the playhead. This is the defect "
                             "that made a looped syllable restart audibly on hardware.")
            fail = True
        else:
            report.append(f"- present: `{name}` does not exist anywhere in main.c's code -- "
                          "that seek cannot happen")

    # A2. the standby chase must be gone with it
    chase_hits = [i for i, l in code_lines if "STANDBY" in l or "standby chase" in l]
    if chase_hits:
        report.append("- **MISSING/BAD**: a standby chase remains at line(s) "
                       + ", ".join(str(h) for h in chase_hits[:5])
                       + " -- it must not be able to run while a valid Stem Tape song is selected")
        fail = True
    else:
        report.append("- present: no standby chase remains in main.c's code -- the track row "
                       "cannot animate an empty-device pattern over a selected song")

    # B. led_service() must not touch pins directly
    led_body = substrings_in_function(lines, func_of_line, "led_service")
    if not led_body:
        report.append("- **MISSING**: led_service() not found")
        fail = True
    else:
        direct = [n for n in LED_FORBIDDEN_IN_LED_SERVICE if n in led_body]
        if direct:
            report.append("- **MISSING/BAD**: led_service() writes LED state directly: "
                           + ", ".join("`" + n + "`" for n in direct)
                           + " -- every pin write belongs to led_apply_frame(), rendering a frame "
                             "st_led_mvp_decide() produced")
            fail = True
        else:
            report.append("- present: led_service() writes no LED mask directly -- it gathers "
                           "state, calls st_led_mvp_decide(), and renders through "
                           "led_apply_frame() alone")
    report.append("")

    # ---- STEM TAPE TRACK-BUTTON PROOF ----
    report.append("### Stem Tape Track button: immediate solo, never a mute")

    scan_body = substrings_in_function(lines, func_of_line, "main")

    # A. tap-to-mute must be unreachable with a stem song selected. The mute
    #    toggle still exists for the classic engine, so its ABSENCE cannot be
    #    required; what must be true is that it sits behind a stem-mode guard.
    if "trk[ti].muted = !trk[ti].muted;" not in scan_body:
        report.append("- present: no tap-to-mute toggle exists at all")
    else:
        idx = scan_body.index("trk[ti].muted = !trk[ti].muted;")
        guard = "atomic_get(&g_stem_song_selected) != 0"
        window = scan_body[max(0, idx - 1600):idx]
        if guard in window:
            report.append("- present: the tap-to-mute toggle is guarded by `" + guard +
                           "` -- a Track tap cannot mutate mute while a Stem Tape song "
                           "is selected")
        else:
            report.append("- **MISSING/BAD**: the tap-to-mute toggle is NOT behind a "
                           "stem-mode guard -- a tap could still latch mute")
            fail = True

    # B. Solo must come from the buttons being DOWN, not from a threshold, and
    #    it must come from the DISPATCHER'S PUBLISHED MASK rather than a
    #    single committed button identity. decode_tracks() names exactly one
    #    button and has no way to express "two pressed", so requiring the mask
    #    here is what keeps multi-stem solo from silently regressing to
    #    one-stem-at-a-time -- and requiring the `if (!stem_ctl)` fence is
    #    what keeps the inherited 700 ms hold path from running alongside it.
    apply_body = substrings_in_function(lines, func_of_line, "stem_ctl_apply")
    if ("trk[k].solo = ((o->track_mask >> k) & 1u) ? 1u : 0u;" in apply_body and
            "if (!stem_ctl) {" in scan_body):
        report.append("- present: in stem mode `trk[k].solo` is assigned from bit k of "
                       "the dispatcher's published Track mask -- solo begins on "
                       "button-down and ends on release, with no TRACK_HOLD_SOLO_MS "
                       "threshold in the path, several stems can be held at once, and "
                       "the inherited hold-to-solo path is fenced off behind "
                       "`if (!stem_ctl)`")
    else:
        report.append("- **MISSING**: stem-mode solo is not driven directly from the "
                       "chord mask's button-down bits -- either the 700 ms hold threshold "
                       "still gates it, or it regressed to a single committed button")
        fail = True

    # B2. The mask the LEDs show must be rebuilt from what the MIXER got.
    if "in.solo_mask |= (uint8_t)(1u << i)" in led_body and "trk[i].solo" in led_body:
        report.append("- present: `led_service()` builds its solo mask from `trk[].solo` "
                       "-- the value that actually reached the channel strip, so what is "
                       "lit cannot drift from what is heard")
    else:
        report.append("- **MISSING**: the LED solo mask is not rebuilt from `trk[].solo` "
                       "-- lights and mixer could disagree")
        fail = True

    # C. selecting a song must clear inherited mute state.
    if "trk[mi].muted = 0u;" in "\n".join(l for _, l in code_lines):
        report.append("- present: song selection clears any inherited `trk[].muted`, so a "
                       "mute latched by earlier firmware cannot survive into Stem Tape")
    else:
        report.append("- **MISSING**: song selection does not clear inherited "
                       "`trk[].muted` -- a stem could stay silent with no gesture to recover it")
        fail = True
    report.append("")

    report.append("## Result")
    report.append("")
    if fail:
        report.append("GATE FAILED -- see missing item(s) above.")
    else:
        report.append("GATE PASSED -- the stored four-stem playback path genuinely calls the real "
                      "st_stem_stream/st_stem_bufmbox/st_stem_mix functions from the real "
                      "audio_thread()/streamer_thread() call sites, not merely linking them "
                      "incidentally.")
    report.append("")

    open(out_path, "w").write("\n".join(report) + "\n")
    print("\n".join(report))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
