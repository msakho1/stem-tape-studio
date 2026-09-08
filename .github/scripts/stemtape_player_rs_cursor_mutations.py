#!/usr/bin/env python3
"""
stemtape_player_rs_cursor_mutations.py -- prove tests/test_rs_cursor_gate.c
actually fails when the extracted cursor implementation is broken.

WHY. A green gate that cannot go red is worth nothing, and this repository has
been bitten by exactly that twice: E-2 printed "runs unconditionally" while
checking three call orderings, and G-4 anchored on a comment banner in a
comment-stripped source and passed for free. The gate this script exercises
makes the strongest claim any gate here has made -- THE OUTPUT DID NOT CHANGE,
sample for sample -- so it had better be able to say the opposite.

WHAT IS MUTATED. The PRODUCTION header, src/st_rs_cursor.h, the same file
main.c includes. Not a model, not the gate's own reference arm. Each mutation
removes one property of the four per-stem cursors and must turn the gate red.

TWO OF THESE ARE THE NEXT COMMIT'S HAZARDS, ON PURPOSE. X-2 collapses the four
cursors onto lane 0 and X-5 collapses the four fractions -- which is precisely
what the planned shared-lane optimisation does, and precisely what it may only
do behind a predicate that has checked the lanes are identical. Pinning them as
RED here, in the commit before that one exists, is the point: the gate that has
to catch a wrong Commit 2 is proven capable of it before Commit 2 is written.

EVERY MUTATION IS IN BOUNDS. A mutation that reads past a group buffer would
"fail" for the wrong reason -- undefined behaviour, not a wrong sample -- so
each one below either keeps the existing clamp or clamps somewhere else. The
gate must fail because the AUDIO differs.

A FINDING FROM THIS RUN, recorded rather than quietly dropped. X-10 was first
written as "the consumed source count is not bounded by the run" -- replacing
`(cur[sp] > src_avail[sp]) ? src_avail[sp] : cur[sp]` with plain `cur[sp]`. It
SURVIVED, and correctly so: `cur` starts at 0, is only ever incremented by one,
and st_rs_cursor_advance() clamps it to exactly `src_avail[sp]` the moment it
reaches it (st_rs_cursor.h lines 189-206). So `cur > src_avail` is
STRUCTURALLY UNREACHABLE and that comparison is dead defensive code, not a
property. It is left in place -- this is firmware with no MMU and the cost is
one compare per run -- but nothing here should claim to test it. X-10 now
mutates something the gate can actually observe.

Three other mutations were rewritten because their first drafts failed only to
COMPILE (unused parameters, an unused local). A mutation that only breaks the
build proves nothing about the gate.

Every mutation must still COMPILE.

Usage:  stemtape_player_rs_cursor_mutations.py [<out-report.md>]
Run from the repository root. Exits non-zero on any hole.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile

GATE = "firmware/stemtape_player/tests/test_rs_cursor_gate.c"
SRC = "firmware/stemtape_player/src"
HDR = f"{SRC}/st_rs_cursor.h"
DEPS = [f"{SRC}/st_planar.c", f"{SRC}/st_pitch.c", f"{SRC}/st_sector_v11.c"]

MUTATIONS = [
    (
        "X-1 the source-bound clamp holds the WRONG frame",
        "\t\tconst uint32_t c = (cur[sp] >= src_avail[sp])\n"
        "\t\t\t\t   ? (src_avail[sp] - 1u) : cur[sp];",
        "\t\tconst uint32_t c = (cur[sp] >= src_avail[sp])\n"
        "\t\t\t\t   ? 0u : cur[sp];",
        "st_rs_out_frames() floors at one output frame, so above 1x a run can "
        "be asked for a source frame it does not hold. Holding the LAST "
        "available frame is a degenerate corner measured in single frames; "
        "holding the FIRST is a jump back to the start of the run.",
    ),
    (
        "X-2 the four cursors are collapsed onto lane 0",
        "\t\tconst uint32_t c = (cur[sp] >= src_avail[sp])\n"
        "\t\t\t\t   ? (src_avail[sp] - 1u) : cur[sp];",
        "\t\tconst uint32_t c = (cur[0] >= src_avail[sp])\n"
        "\t\t\t\t   ? (src_avail[sp] - 1u) : cur[0];",
        "THE NEXT COMMIT'S HAZARD, PINNED EARLY. Sharing one cursor across "
        "four lanes is correct exactly while the lanes are identical and "
        "silently wrong the moment one is not -- a reversed stem, a starved "
        "stem, or a stem that has just left reverse. It must never be done "
        "without a predicate that has checked.",
    ),
    (
        "X-3 the four-index decode is collapsed to the shared one",
        "\tif (idx[0] == idx[1] && idx[1] == idx[2] &&\n"
        "\t    idx[2] == idx[3]) {\n"
        "\t\tst_pl_decode_frame_shared(grp, idx[0], nxt);\n"
        "\t} else {\n"
        "\t\tst_pl_decode_frame(grp, idx, nxt);\n"
        "\t}",
        "\tst_pl_decode_frame_shared(grp, idx[0], nxt);",
        "Every stem would be decoded at stem 0's position. This is the whole "
        "reason st_pl_decode_frame()'s array form exists: per-track reverse "
        "gives one head a genuinely different index.",
    ),
    (
        "X-4 interpolator validity is taken from lane 0 for all four",
        "\t\tif (!prev_valid[sp]) {",
        "\t\tif (!prev_valid[0]) {",
        "st63 clears exactly ONE lane's validity when that stem leaves "
        "reverse and rejoins the master. Reading lane 0's flag either fails to "
        "prime the rejoining lane -- which then interpolates from audio at a "
        "position it no longer occupies -- or re-primes three lanes that never "
        "left, flattening their first output frame.",
    ),
    (
        "X-5 the four cursor fractions are collapsed onto lane 0",
        "\t\tfrac[sp] += rate_q16;\n"
        "\t\twhile (frac[sp] >= ST_RS_ONE) {\n"
        "\t\t\tfrac[sp] -= ST_RS_ONE;",
        "\t\tfrac[sp] = frac[0] + rate_q16;\n"
        "\t\twhile (frac[sp] >= ST_RS_ONE) {\n"
        "\t\t\tfrac[sp] -= ST_RS_ONE;",
        "THE NEXT COMMIT'S OTHER HAZARD. main.c's reverse-release path clears "
        "ONLY the rejoining lane's s_stem_rate_frac[j], so immediately after a "
        "release the four heads are co-located and forward -- `together` is "
        "true -- while one carries fraction 0 and three carry a fraction. A "
        "shared fraction renders that stem at the other three's sub-sample "
        "phase.",
    ),
    (
        "X-6 the out-of-run corner keeps a fraction above 1.0",
        "\t\t\t\tfrac[sp] &= (ST_RS_ONE - 1u);",
        "\t\t\t\tfrac[sp] = frac[sp];",
        "Leaving frac above 1.0 makes the next blend EXTRAPOLATE past both of "
        "its samples instead of interpolating between them.",
    ),
    (
        "X-7 the out-of-run corner reports one frame too few",
        "\t\t\t\tcur[sp] = src_avail[sp];",
        "\t\t\t\tcur[sp] = src_avail[sp] - 1u;",
        "used_out[] is what each stream is advanced by. One frame short, every "
        "run, is a playhead that falls behind the audio it produced -- the "
        "exact drift the 'reported rather than recomputed' rule exists to "
        "prevent.",
    ),
    (
        "X-8 the frame behind the new cursor is never re-decoded",
        "\t\t\t\tif (pidx == idx[sp]) {",
        "\t\t\t\tif (pidx == idx[sp] || 1) {",
        "The shortcut is only valid on the FIRST step of the walk, where the "
        "frame behind the new cursor is the one just decoded. At rates that "
        "cross two source frames per output frame the second step must decode "
        "for real, or the blend spans a gap it never looked at.",
    ),
    (
        "X-9 the walk looks at the cursor, not the frame behind it",
        "\t\t\t\tuint32_t pc = cur[sp] - 1u;",
        "\t\t\t\tuint32_t pc = cur[sp];",
        "`prev` would hold the frame AT the new cursor rather than behind it, "
        "so the next output frame blends a sample with itself: the "
        "interpolation collapses and the resampler becomes a nearest-neighbour "
        "decimator.",
    ),
    (
        "X-10 the consumed source count is collapsed onto lane 0",
        "\t\tused_out[sp] = (cur[sp] > src_avail[sp]) ? src_avail[sp] : cur[sp];",
        "\t\tused_out[sp] = (cur[0] > src_avail[sp]) ? src_avail[sp] : cur[0];",
        "used_out[] is what each stream is advanced by, and after per-track "
        "reverse the four counts are genuinely different numbers. Publishing "
        "lane 0's for all four advances three playheads by a distance they did "
        "not travel -- silent desynchronisation, not a dropout.",
    ),
    (
        "X-11 the carried fraction is collapsed onto lane 0",
        "\t\tfrac_io[sp] = frac[sp];",
        "\t\tfrac_io[sp] = frac[0];",
        "The sub-frame phase must survive a run boundary PER HEAD. Publishing "
        "lane 0's for all four is a position step on three heads several times "
        "a block, and it is the writeback-side form of the same collapse X-5 "
        "makes inside the walk -- the next commit has to get both right or "
        "neither.",
    ),
]


def build_and_run(root: str) -> tuple[bool, str]:
    """Returns (gate_passed, detail). A build failure is NOT a pass."""
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
    tail = [ln for ln in run.stdout.strip().split("\n") if ln][-1:]
    return run.returncode == 0, (tail[0] if tail else "<no output>")


def main() -> int:
    report = ["# Stem Tape resampler-cursor extraction -- mutation coverage", ""]
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
        report.append("Every mutation below is applied to the PRODUCTION header "
                      "`src/st_rs_cursor.h`, the same file main.c includes.")
        report.append("")
        report.append("| # | mutation | gate | what it would ship |")
        report.append("|---|---|---|---|")

        holes = 0
        for name, old, new, why in MUTATIONS:
            work = os.path.join(base, "work")
            if os.path.exists(work):
                shutil.rmtree(work)
            shutil.copytree(clean, work)

            path = os.path.join(work, HDR)
            text = open(path).read()
            n = text.count(old)
            if n != 1:
                print(f"FATAL: {name}: anchor occurs {n} times in {HDR}, "
                      f"want exactly 1", file=sys.stderr)
                return 1
            open(path, "w").write(text.replace(old, new))

            passed, detail = build_and_run(work)
            if passed:
                holes += 1
                verdict = "**STILL PASSES -- HOLE**"
            else:
                verdict = "red"
            print(f"{'HOLE' if passed else 'ok  '}  {name}: {verdict} ({detail})")
            report.append(f"| {name.split()[0]} | {' '.join(name.split()[1:])} "
                          f"| {verdict} | {why} |")

    report.append("")
    if holes:
        report.append(f"**{holes} mutation(s) survived.** The gate does not "
                      f"actually test what it claims.")
    else:
        report.append(f"All {len(MUTATIONS)} mutations turn the gate red, and "
                      f"all of them mutate PRODUCTION code rather than a model "
                      f"of it -- which is the whole reason the cursors were "
                      f"moved into a header this gate can link.")
        report.append("")
        report.append("What it still does not prove: that main.c CALLS these "
                      "helpers, in this order. That is the wiring check "
                      "(I-1/I-2), which reads production main.c directly.")

    if len(sys.argv) > 1:
        with open(sys.argv[1], "w", encoding="utf-8") as fh:
            fh.write("\n".join(report) + "\n")
    print()
    print("\n".join(report))
    return 1 if holes else 0


if __name__ == "__main__":
    sys.exit(main())
