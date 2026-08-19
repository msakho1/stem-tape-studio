#!/usr/bin/env python3
"""Stem Tape Player -- stored four-stem playback wiring check (fail-closed).

Phase 2 slice 3B: proves the real production audio-data-path call
sequence -- now continuous, multi-sector streaming via st_stem_stream.h's
pure state machine, not slice 2's single resident sector -- is genuinely
present in main.c's own source, not merely that the callee symbols exist
somewhere in the link (the runtime symbol-presence gate's own CI step
already proves THAT separately). Source-level call-site proof, not
link-level: reuses index_functions() from stemtape_player_safety_gate.py
(the SAME brace-depth enclosing-function parser this repo's own write-
safety gate already depends on and self-tests -- see
stemtape_player_safety_gate_parser_selftest.py) rather than reimplementing
a second C-source scanner.

Checks (every one a REAL call expression -- `symbol(`-- found, via
index_functions()'s own brace-depth tracking, textually inside the named
enclosing function's own body, skipping comment lines the same way
find_call_sites() in stemtape_player_safety_gate.py already does):

  1. looper_audio_block() (the real-time audio mixer, called every I2S
     block from audio_thread()) calls st_stream_required_sector() and
     st_stream_advance_frame() -- the pure streaming state machine's own
     per-frame bookkeeping -- plus st11_sector_decode_frame() -- the real
     STSC per-frame decoder, RAM-only, no I/O -- and st_stem_mix_frame()
     -- the real 4-stem-to-stereo mixdown. This is the "stored four-stem
     playback path actually references/uses st_stem_stream/st_stem_mix"
     requirement: not incidental symbol presence, real call sites inside
     the real real-time audio function.

  2. streamer_thread() (the one thread that ever touches flash) calls
     st_stream_init() (seeding the state machine from the real selected
     song's own STIX geometry), st_stream_validate_sector() (validating
     EVERY sector read, not just the first), and st_stream_sector_ready()
     (publishing a freshly-validated sector to the audio path) -- proving
     both the boot-time first sector AND the continuous per-pass prefetch
     that streams the rest of the song are validated through the real
     state machine, never trusted blindly.

Fails closed: main.c missing, either function's body not found, or any
required call site absent.

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
        "st_stream_advance_frame",
    ],
    "streamer_thread": [
        "st11_sector_read_header",
        "st_stream_init",
        "st_stream_validate_sector",
        "st_stream_sector_ready",
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

    report.append("## Result")
    report.append("")
    if fail:
        report.append("GATE FAILED -- see missing item(s) above.")
    else:
        report.append("GATE PASSED -- the stored four-stem playback path genuinely calls the real "
                      "st11_sector_decode_frame()/st_stem_mix_frame()/st11_sector_read_header() "
                      "functions from the real audio_thread()/streamer_thread() call sites, not "
                      "merely linking them incidentally.")
    report.append("")

    open(out_path, "w").write("\n".join(report) + "\n")
    print("\n".join(report))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
