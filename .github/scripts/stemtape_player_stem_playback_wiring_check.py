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

  1. looper_audio_block() (the real-time audio mixer, called every I2S
     block from audio_thread()) -- the mailbox CONSUMER -- calls
     st_stream_required_sector()/st_stream_sector_ready()/st_stream_
     advance_frame() (the pure streaming state machine's own per-frame
     bookkeeping, exclusively audio-thread-owned as of this slice) plus
     st_stem_mbox_try_acquire()/st_stem_mbox_set_requested_sector() (the
     atomic mailbox's consumer-side API) plus st11_sector_decode_frame()
     -- the real STSC per-frame decoder, RAM-only, no I/O -- and
     st_stem_mix_frame() -- the real 4-stem-to-stereo mixdown. This is
     the "stored four-stem playback path actually references/uses
     st_stem_stream/st_stem_bufmbox/st_stem_mix" requirement: not
     incidental symbol presence, real call sites inside the real real-
     time audio function.

  2. streamer_thread() (the one thread that ever touches flash) -- the
     mailbox PRODUCER -- calls st_stream_init() (seeding the state
     machine from the real selected song's own STIX geometry, once, at
     boot) and st_stream_validate_sector() (validating EVERY sector
     read, not just the first -- read-only geometry access, safe from
     the producer thread) plus st_stem_mbox_init()/st_stem_mbox_
     producer_next_fill()/st_stem_mbox_publish_ready() (the atomic
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

  5. Phase 3 control-matrix, LED slice: led_service() (the single real
     owner of the physical LEDs -- see its own doc comment) reads the
     SAME trk[].muted/trk[].solo state as check 4, gated on the SAME
     g_stem_song_selected flag PASS C itself gates on, calls the real
     st_stem_mix_channel_audible() (REQUIRED_CALLS -- the SAME shared
     audibility formula looper_audio_block()'s own mixer uses
     internally, so LED feedback can never drift from what the mixer
     actually plays), and drives the real track_led_on()/track_led_
     ghost()/track_led_off() primitives -- proving stem mute/solo status
     reaches the physical LEDs, not just the mixer.

  6. Phase 3 control-matrix, beat-sync LED slice: streamer_thread()'s
     boot code genuinely reads lib.active.bpm_q8/downbeat_frame (the
     selected STIX record's own authoritative song-level timing) AND
     cross-checks them against hdr.bpm_q8/hdr.downbeat_frame (the first
     sector's own header) before calling the real st_beat_timing_init()
     (REQUIRED_CALLS); looper_audio_block() genuinely calls atomic_set(
     &g_stem_song_frame_pub, ...) once per block (REQUIRED_SUBSTRINGS --
     an atomic-write call whose OWN argument is what proves it is the
     right one, not just any atomic_set); led_service() genuinely reads
     atomic_get(&g_stem_peak_pub[i]) and calls the real
     st_stem_meter_update()/st_stem_meter_brightness() (REQUIRED_CALLS)
     -- proving the whole chain from the decoded stem samples through
     to the physical per-LED brightness is real, not merely linked.

     BEAT PULSE, CORRECTED: this gate previously required led_service()
     to call st_beat_phase_on_beat()/st_beat_led_decide(). That display
     derived ONE boolean from the STIX tempo and handed the SAME value
     to all four track LEDs, so every audible stem lit and darkened
     together -- uniform by construction, carrying no per-stem
     information. It is replaced by per-stem output-level metering
     (src/st_stem_meter.c), so those two calls are gone from
     led_service() and this gate now requires the calls that actually
     drive the lights.

     st_beat_timing_init() is still called and still required above (the
     selected song's tempo is still parsed and held): what changed is
     only that the LED row stopped being driven by a beat boolean.
     st_beat_phase_on_beat()/st_beat_led_decide() therefore currently
     have no caller -- stated here rather than left to be discovered,
     since this repo's rule is that unwired code must not be presented
     as part of the real runtime. They are retained, with their host
     tests, for the loop-quantization work (CTL-04/CTL-12/CTL-22) that
     consumes the same bar/beat derivation; if that work does not land,
     they should be deleted rather than kept indefinitely.

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
    "looper_audio_block": [
        "st11_sector_decode_frame",
        "st_stem_mix_frame",
        "st_stream_required_sector",
        "st_stream_sector_ready",
        "st_stream_advance_frame",
        "st_stem_mbox_try_acquire",
        "st_stem_mbox_set_requested_sector",
    ],
    "streamer_thread": [
        "st11_sector_read_header",
        "st_stream_init",
        "st_stream_validate_sector",
        "st_stem_mbox_init",
        "st_stem_mbox_producer_next_fill",
        "st_stem_mbox_publish_ready",
        "st_beat_timing_init",
    ],
    "audio_thread": [
        "looper_audio_block",
        "i2s_write",
    ],
    "main": [
        "st_track_hold_tick",
    ],
    "led_service": [
        "st_stem_mix_channel_audible",
        "st_stem_meter_update",
        "st_stem_meter_brightness",
    ],
}

# Substring checks (see REQUIRED_CALLS's doc comment, check 4): these are
# array-field reads/assignments, not call expressions, so they cannot be
# found by calls_in_function()'s `name(` regex -- a plain per-line substring
# search within the function's own body (skipping comment-only lines, same
# as calls_in_function()) is enough, since each string here is specific
# enough not to appear incidentally in unrelated code or in a real (non-
# comment) statement other than the one it is meant to prove.
REQUIRED_SUBSTRINGS = {
    "looper_audio_block": [
        "trk[s].vol_q8",
        "trk[s].muted",
        "trk[s].solo",
        "atomic_set(&g_stem_song_frame_pub",
    ],
    "main": [
        "track_hold[ti].solo_active",
    ],
    "led_service": [
        "atomic_get(&g_stem_song_selected)",
        "trk[i].solo",
        "trk[i].muted",
        "track_led_on(i)",
        "track_led_ghost(i)",
        "track_led_off(i)",
        # Beat pulse is now per-stem: led_service() reads each stem's own
        # published peak instead of the song-position mirror and the shared
        # tempo record. See this file's own "BEAT PULSE, CORRECTED" note.
        # The [i] subscript is part of the required string on purpose -- it
        # proves the PER-STEM array is being read, not some single scalar.
        "atomic_get(&g_stem_peak_pub[i])",
        "g_trk_level[i]",
    ],
    "streamer_thread": [
        "lib.active.bpm_q8",
        "lib.active.downbeat_frame",
        "hdr.bpm_q8",
        "hdr.downbeat_frame",
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
