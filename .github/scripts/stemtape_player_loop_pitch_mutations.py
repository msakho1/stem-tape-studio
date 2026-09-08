#!/usr/bin/env python3
"""
stemtape_player_loop_pitch_mutations.py -- prove tests/test_loop_pitch_gate.c
actually fails when the authoritative loop-wrap participation guard is broken.

WHY. A green gate that cannot go red is worth nothing, and this repository has
been bitten by exactly that twice: E-2 printed "runs unconditionally" while
checking three call orderings, and G-4 anchored on a comment banner in a
comment-stripped source and passed for free. The guard being fixed here is one
`if`; the whole correctness of loop-at-pitch now rests on it, so every way of
getting it wrong has to be shown red.

Six mutations, matching the six failure modes:

  M-1  restore the old positional equality rule
  M-2  drop the reverse exclusion (a reversed stem gets dragged)
  M-3  let a START_OF_SONG parked head participate
  M-4  let an END_OF_SONG parked head participate
  M-5  skip one ordinary forward lane
  M-6  seek every head unconditionally, reversed lane included

RECORDED HONESTLY: M-3 and M-4 are DEFENCE IN DEPTH. A head parked at either
end of the song with `reverse` already cleared is not reachable through today's
control flow -- a START_OF_SONG park implies a reversed head, and st63's
release seeks the leaver out of it before clearing reverse. Those two
conditions are therefore not load-bearing in production TODAY. The gate
constructs the state directly so the mutations are still killed rather than
surviving as equivalent mutants, and this note exists so nobody later reads a
red M-3 as evidence the state is reachable.

Every mutation must still COMPILE.

Usage:  stemtape_player_loop_pitch_mutations.py [<out-report.md>]
Run from the repository root. Exits non-zero on any hole.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile

SRC = "firmware/stemtape_player/src"
GATE = "firmware/stemtape_player/tests/test_loop_pitch_gate.c"
DEPS = [f"{SRC}/st_stem_stream.c", f"{SRC}/st_pitch.c", f"{SRC}/st_planar.c",
        f"{SRC}/st_sector_v11.c", f"{SRC}/st_crc32.c", f"{SRC}/st_checksum32.c"]

GUARD = """	if (hd[k].reverse) {
		return false;                               /* independent by intent */
	}
	if (hd[k].state == ST_STREAM_START_OF_SONG ||
	    hd[k].state == ST_STREAM_END_OF_SONG) {
		return false;                               /* parked: consumes no source */
	}
	return true;"""

PARTICIPATE_CALL = "				if (!participates(k, tr)) { continue; }"

MUTATIONS = [
    (
        "M-1 the old positional equality rule is restored",
        GUARD,
        "	return hd[k].song_frame == tr->song_frame;",
        "THE DEFECT ITSELF. Off unity the run clamp lands all four on loop_end "
        "together, the backstop wraps the three non-transport heads "
        "immediately, and by the time the duck fires they no longer match the "
        "transport -- so only the transport moves. d * (1 - 1/rate) of "
        "permanent, cumulative offset per wrap: the flam, the shared-lane "
        "collapse that produced the crackle and the apparent BPM drop, and the "
        "starved stem that never came back.",
    ),
    (
        "M-2 the reverse exclusion is dropped",
        "	if (hd[k].reverse) {\n"
        "		return false;                               /* independent by intent */\n"
        "	}\n",
        "",
        "A deliberately reversed stem would be dragged back to loop_start on "
        "every wrap, destroying per-track reverse inside a loop. This is the "
        "case the original positional guard existed to protect, and replacing "
        "a proxy with explicit state must not lose it.",
    ),
    (
        "M-3 a START_OF_SONG parked head participates",
        "	if (hd[k].state == ST_STREAM_START_OF_SONG ||\n"
        "	    hd[k].state == ST_STREAM_END_OF_SONG) {",
        "	if (hd[k].state == ST_STREAM_END_OF_SONG) {",
        "A head parked at the front of the song consumes no source and must "
        "not be seeked -- st64's rule, restated. DEFENCE IN DEPTH: not "
        "reachable today with reverse already cleared (see this script's "
        "header); the gate constructs it.",
    ),
    (
        "M-4 an END_OF_SONG parked head participates",
        "	if (hd[k].state == ST_STREAM_START_OF_SONG ||\n"
        "	    hd[k].state == ST_STREAM_END_OF_SONG) {",
        "	if (hd[k].state == ST_STREAM_START_OF_SONG) {",
        "The mirror of M-3, and the same st64 rule. DEFENCE IN DEPTH, same "
        "caveat.",
    ),
    (
        "M-5 one ordinary forward lane is skipped by the authoritative jump",
        PARTICIPATE_CALL,
        PARTICIPATE_CALL + "\n"
        "				if (k == STEMS - 1u) { continue; }",
        "The invariant is that ALL ordinary forward stems take part in ONE "
        "authoritative wrap. Leaving any single lane out reproduces the "
        "original defect for that lane -- which is precisely how one stem "
        "ended up permanently displaced on hardware.",
    ),
    (
        "M-6 every head is seeked unconditionally, reversed lane included",
        PARTICIPATE_CALL,
        "				(void)participates(k, tr);   /* verdict ignored */",
        "The lazy 'fix': drag everything to loop_start. It makes the forward "
        "stems agree and silently destroys reverse -- a generic resync, which "
        "is exactly what this change is NOT.",
    ),
]


def build_and_run(root: str) -> tuple[bool, str]:
    binp = os.path.join(root, "gate")
    cc = subprocess.run(
        ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
         "-I" + os.path.join(root, SRC)]
        + [os.path.join(root, d) for d in DEPS]
        + [os.path.join(root, GATE), "-o", binp],
        capture_output=True, text=True)
    if cc.returncode != 0:
        return False, "did not compile: " + cc.stderr.strip().split("\n")[0]
    run = subprocess.run([binp], capture_output=True, text=True, cwd=root)
    tail = [ln for ln in run.stdout.strip().split("\n") if ln]
    detail = next((ln for ln in reversed(tail) if "failures" in ln),
                  tail[-1] if tail else "<no output>")
    return run.returncode == 0, detail


def main() -> int:
    report = ["# Stem Tape loop-at-pitch wrap guard -- mutation coverage", ""]
    repo = os.getcwd()

    with tempfile.TemporaryDirectory() as base:
        clean = os.path.join(base, "clean")
        shutil.copytree(os.path.join(repo, "firmware"),
                        os.path.join(clean, "firmware"))

        ok, detail = build_and_run(clean)
        if not ok:
            print(f"FATAL: the UNMUTATED gate does not pass: {detail}",
                  file=sys.stderr)
            return 1
        report.append(f"Baseline (unmutated): **passes** -- `{detail}`")
        report.append("")
        report.append("| # | mutation | gate | what it would ship |")
        report.append("|---|---|---|---|")

        holes = 0
        for name, old, new, why in MUTATIONS:
            work = os.path.join(base, "work")
            if os.path.exists(work):
                shutil.rmtree(work)
            shutil.copytree(clean, work)

            path = os.path.join(work, GATE)
            text = open(path).read()
            n = text.count(old)
            if n != 1:
                print(f"FATAL: {name}: anchor occurs {n} times, want exactly 1",
                      file=sys.stderr)
                return 1
            open(path, "w").write(text.replace(old, new))

            passed, detail = build_and_run(work)
            if passed:
                holes += 1
                verdict = "**STILL PASSES -- HOLE**"
            else:
                verdict = f"red ({detail})"
            print(f"{'HOLE' if passed else 'ok  '}  {name}: {verdict}")
            report.append(f"| {name.split()[0]} | {' '.join(name.split()[1:])} "
                          f"| {verdict} | {why} |")

    report.append("")
    if holes:
        report.append(f"**{holes} mutation(s) survived.** The gate does not "
                      f"actually test what it claims.")
    else:
        report.append(f"All {len(MUTATIONS)} mutations turn the gate red.")
        report.append("")
        report.append("M-3 and M-4 are DEFENCE IN DEPTH and the gate "
                      "constructs their state: a head parked at either end of "
                      "the song with `reverse` already cleared is not "
                      "reachable through today's control flow. They are killed "
                      "rather than equivalent, and nobody should read a red "
                      "M-3 as evidence that the state is reachable.")
        report.append("")
        report.append("What none of it proves: that production main.c uses "
                      "this predicate. That is the wiring check (J-1/J-2), "
                      "which reads main.c directly.")

    if len(sys.argv) > 1:
        with open(sys.argv[1], "w", encoding="utf-8") as fh:
            fh.write("\n".join(report) + "\n")
    print()
    print("\n".join(report))
    return 1 if holes else 0


if __name__ == "__main__":
    sys.exit(main())
