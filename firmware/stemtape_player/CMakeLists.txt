cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(stemtape-player)

include(${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake)

# src/main.c and src/sp1_emmc.c are real derivatives of the proven Tape
# Looper (firmware/src/main.c, firmware/src/sp1_emmc.c) -- see main.c's
# provenance banner.
#
# GATE 1: src/st_midi_queue.c is the pure, host-testable USB-MIDI receive
# decode + bounded event queue (see st_midi_queue.h). main.c's real
# rx_packet_cb is a thin adapter that calls st_midi_decode_ump32() on the
# incoming UMP word -- the SAME function tests/test_stemtape_player.c
# exercises on the host -- so this is genuinely linked, not a parallel
# unwired module.
#
# STEM TAPE v1.1 (docs/stem-tape-transfer-v1.1.md is the current,
# authoritative contract): src/st_crc32.c, src/st_checksum32.c,
# src/st_sector_v11.c, src/st_stix.c, src/st_stcp.c, src/st_ab_session.c --
# the real STIX v2 index parse/validate/select, STSC sector codec, A/B
# region layout + STCP capability reply, and the stateful write-safety
# session gate (see each header's own doc) -- are linked: main.c's real
# 'Q' handler and boot-time A/B selection call these SAME pure functions
# tests/test_stem_v11.c exercises against the frozen Lovable handoff
# fixtures. st_crc32.c is linked for st_stix.c's own record-integrity CRC
# (st_stix_block_crc()), not for any v1.0 purpose.
#
# Phase 2 slice 2: src/st_stem_mix.c -- the pure, real-time-safe 4-stem-
# to-stereo mixdown (see st_stem_mix.h's own doc) -- is now linked and
# genuinely called: main.c's looper_audio_block() (PASS C) calls
# st_stem_mix_frame() on frames st11_sector_decode_frame() decodes from a
# real stored song's own sector data, feeding the SP-1's existing physical
# I2S output. Was added unlinked, host-test-only, in an earlier commit
# (see git log); the slice-2 commit gave it its real caller, per this
# file's own established policy below.
#
# Phase 2 slice 3B: src/st_stem_stream.c -- the pure stored-song
# CONTINUOUS streaming state machine (see st_stem_stream.h's own doc) --
# is now linked and genuinely called: streamer_thread() seeds it from the
# selected song's own real STIX geometry and validates every sector (not
# just the first) through it; audio_thread (looper_audio_block()'s own
# PASS C) is its EXCLUSIVE owner as of slice 3B.1's concurrency
# correction, driving its per-frame song-position/sector-readiness/
# underrun bookkeeping. Was added unlinked, host-test-only, in slice 3A
# (see git log); slice 3B gave it its real caller, per this file's own
# established policy.
#
# Phase 2 slice 3B.1 (concurrency correction): src/st_stem_bufmbox.c --
# the formally specified, lock-free single-producer/single-consumer
# double-buffer publication mailbox (see st_stem_bufmbox.h's own full
# protocol spec) -- replaces slice 3B's `volatile`-fields-plus-
# documented-write-order cross-thread handoff between streamer_thread
# (producer) and audio_thread (consumer) with real atomics and acquire/
# release ordering. Compiled with -DST_STEM_BUFMBOX_ZEPHYR (below) so its
# atomic primitives resolve to Zephyr's own atomic_t, unconditionally
# sequentially consistent on every architecture Zephyr supports --
# strictly stronger than the acquire/release the protocol requires. The
# SAME source file, built instead against C11 <stdatomic.h> with the
# minimal acquire/release ordering the protocol actually needs, is what
# tests/test_stem_bufmbox.c's real concurrent-pthread host tests exercise
# (see that file's own doc comment for why that is a faithful stand-in,
# and where the faithfulness gap actually is).
#
# Phase 3 control-matrix (momentary hold-to-solo, corrected): src/
# st_track_hold.c -- the pure, host-tested momentary hold-timing state
# machine (see st_track_hold.h's own doc comment for the semantics, and
# why this replaced a first version that wrongly toggled solo once at
# release -- a latch, not the momentary behavior actually specified) --
# is linked and genuinely called: main.c's own track-button scan loop
# ticks one instance per track every scan pass, and looper_audio_block()/
# led_service() both read the resulting trk[].solo state it drives.
#
# Phase 3 control-matrix (beat-sync LED slice): src/st_beat_phase.c -- the
# pure, host-tested, RAM-only beat-phase computation (see that file's own
# doc comment: the selected STIX record's own bpm_q8/downbeat_frame are
# the sole authoritative tempo source, sector-header timing is cross-
# checked for consistency only, and phase is always re-derived from the
# master song_frame position's own atomic mirror -- never a second,
# independently-ticking clock) -- is linked and genuinely called: main.c's
# own streamer_thread() boot code publishes the timing snapshot once (from
# the SAME already-read STIX record slice 3B/3B.1 already consume for
# geometry), audio_block_epilogue() publishes the song-position mirror once
# per block, and led_service() calls st_beat_pulse() on both of those every
# control pass. EVERY function in this file has a real caller: the module
# once also exported st_beat_phase_on_beat()/st_beat_led_decide(), which
# lost their caller when the eight LEDs got a single semantic owner, and
# have since been DELETED rather than left compiled in and unreachable.
#
# The OLD, now-retired v1.0 Gate 2 contract's own engine -- src/
# st_storage_layout.c, src/st_sector_codec.c, src/st_transfer.c, src/
# st_library_io.c, src/st_xfer_wire.c, and src/st_stem_validate.c (an
# even earlier, already-superseded design within that same v1.0 line) --
# is DELIBERATELY NOT linked here: main.c no longer calls any command
# handler or adapter that reaches these files' functions at all (the
# v1.1 migration deleted that dispatch code, not merely disabled it), so
# there is no path from this target's command dispatch to their own
# emmc_write_blocks() call sites, and no v1.0 write path is ever present
# in this ELF -- v1.0 and v1.1 can never both hold a live write path in
# the same build, by construction. These files stay in the tree and keep
# their own host tests (tests/test_stemtape_player.c) for regression
# evidence only; a follow-up commit extends the strict persistence safety
# gate to check their symbols are absent from the built ELF directly, as
# positive proof rather than relying on this list alone.
#
# NOT included here (compiled only into tests/test_stemtape_player.c's host
# test binary, not into this firmware target): led_duty.c/led_render.c/
# led_render_policy.c, st_gesture.c, st_scrub.c, st_led_pattern.c --
# st_gesture.c/st_scrub.c/st_led_pattern.c implemented an earlier,
# superseded standalone protocol/runtime design (see git log); main.c
# still drives LEDs directly (its own GPIO+soft-PWM driver, inherited
# from the Tape Looper). Preserved and still host-tested for reference;
# deliberately NOT linked into this ELF so as not to present unwired
# modules as part of the real runtime.
#
# Upload-reliability phase, Slice C2: src/st_bulk_xfer.c -- the pure wire-
# contract parser/builder + per-session sequence state machine behind the
# new 'U' bulk verified-sector command (see st_bulk_xfer.h's own doc and
# docs/stem-tape-bulk-upload-v1.md) -- is now linked and genuinely called:
# main.c's real xfer_bulk_write_sector() (xfer_service()'s 'U' handler)
# calls st_bulk_parse_header()/st_bulk_build_response()/st_bulk_seq_check()/
# st_bulk_seq_advance() against the real CDC/eMMC environment. Was added
# unlinked, host-test-only, in Slice C1 (see git log); this slice gave it
# its real caller, per this file's own established policy (see
# st_stem_stream.c's own identical slice-3A/3B split above).
# Tape transport inertia: src/st_inertia.c -- the pure spin-up/spin-down
# envelope. It is a 0..1 multiplier on the transport rate and knows nothing
# about audio; main.c's stem run loop applies it to the shared playhead and
# src/st_resample.h (header-only, no translation unit) converts between the
# source-frame and output-frame domains that a rate other than 1x separates.
# Host-tested in tests/test_inertia.c.

# Rocker semitone control: src/st_pitch.c -- the song's pitch, on the same
# equal-tempered 2^(k/12) grid the inherited Tape Looper's own semitone
# double-click uses (firmware/src/main.c's k_semi_q16), subdivided to half
# steps. It is a tape varispeed: the value is a playback RATE, so pitch and
# time move together exactly as they do in the Looper, and no time-stretching
# was added. Host-tested in tests/test_pitch.c, which re-derives the Looper's
# whole-semitone table out of this one and compares it entry by entry.

# LED MVP (single semantic owner): src/st_led_mvp.c -- the pure,
# host-tested decision that turns real runtime state into one complete
# eight-LED frame, plus the local battery classifier (see st_led_mvp.h) --
# is linked and genuinely called: main.c's led_service() fills
# st_led_inputs_t from the live transport/mixer/charger state, calls
# st_led_mvp_decide(), and renders the result through the existing
# TIMER3/GPIO soft-PWM masks. It is the ONLY thing that writes those masks
# in normal running.
#
# RE-LINKED: src/st_stem_meter.c -- the per-stem envelope follower and
# brightness curve, one instance per stem.
#
# It was unlinked once, on the reasoning that "a level meter is not the
# stable readable indication the LED contract specifies -- a quiet passage
# dimmed a stem that was audible". That reasoning is reversed on the
# product's own terms: a quiet passage SHOULD dim its stem, because the Track
# row's job is to show what each part of the arrangement is doing, and a row
# that reads the same whether a stem is playing or resting shows nothing at
# all. The stable indication the contract wanted is the SIDE row, which still
# has it -- S4 carries the beat, unchanged.
#
# The module owns no clock and takes no tempo input; it converts a stream of
# per-stem peaks into a brightness. Host-tested in tests/test_stem_meter.c
# (the arithmetic) and tests/test_led_audio_reactive.c (the five acceptance
# behaviours, against synthesised drum/pad/vocal material).
target_sources(app PRIVATE
	src/main.c
	src/sp1_emmc.c
	src/st_midi_queue.c
	src/st_crc32.c
	src/st_checksum32.c
	src/st_sector_v11.c
	# v1.2 song-planar addressing. main.c's read path calls st_pl_group_block(),
	# st_pl_validate() and st_pl_decode_frame() on every fetch and every frame,
	# so this is not optional -- omitting it links to nothing and the build dies
	# at the first link pass with no zephyr.elf, which is exactly what happened.
	src/st_planar.c
	src/st_stix.c
	src/st_stcp.c
	src/st_ab_session.c
	src/st_bulk_xfer.c
	src/st_stem_mix.c
	src/st_stem_stream.c
	src/st_stem_bufmbox.c
	src/st_track_hold.c
	src/st_ladder.c
	src/st_ctl.c
	src/st_scratch.c
	src/st_loop.c
	src/st_fx.c
	src/st_fx_ctl.c
	src/st_inertia.c
	src/st_pitch.c
	src/st_fnplay.c
	src/st_readcost.c
	src/st_beat_phase.c
	src/st_stem_meter.c
	src/st_led_mvp.c
)

# See firmware/CMakeLists.txt's identical note: the batchbuf/meta-block code
# (inherited unchanged in this derivative) type-puns static uint8_t[]
# storage through other lvalue types; -fno-strict-aliasing removes the TBAA
# assumption that would make that idiom undefined.
target_compile_options(app PRIVATE -fno-strict-aliasing)

# Selects st_stem_bufmbox.h's real-Zephyr-atomic_t backend for this
# firmware target only -- the host test build (plain `cc`, no Zephyr
# headers available) never defines this, so it takes the C11
# <stdatomic.h> backend instead. See st_stem_bufmbox.h's own doc comment.
target_compile_definitions(app PRIVATE ST_STEM_BUFMBOX_ZEPHYR)

target_link_libraries(app PRIVATE m)

# ---- AIN1 CALIBRATION CAPTURE ------------------------------------------
# OFF unless -DST_VOL_CAL=1 is passed on the west/cmake command line. An
# explicit hook rather than EXTRA_COMPILE_DEFINITIONS, which is not a
# dependable Zephyr variable across versions.
#
# The capture prints the raw AIN1 rail so the Volume-/Volume+ chord value can
# be measured; the FX overlay cannot decode that chord until it is. It must
# never reach a shipped image, which two independent checks enforce: the
# safety gate greps main.c's own #define, and the CI build compares `strings`
# on the two BINs.
if(ST_VOL_CAL)
  zephyr_compile_definitions(ST_VOL_CAL=1)
  message(STATUS "AIN1 CALIBRATION CAPTURE ENABLED -- not a shippable image")
endif()
