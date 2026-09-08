#!/usr/bin/env python3
"""
stemtape_player_resample_lock_mutations.py -- prove
tests/test_resample_lock_gate.c actually fails when the shared-lane cursor is
broken, in each of the eight ways it can be broken.

WHY. A green gate that cannot go red is worth nothing, and this repository has
been bitten by exactly that twice: E-2 printed "runs unconditionally" while
checking three call orderings, and G-4 anchored on a comment banner in a
comment-stripped source and passed for free. The shared lane's whole claim is
"bit-identical, and only when that is provable", so both halves have to be
falsifiable: the predicate must be SOUND (L-1..L-4, L-7), the broadcast must be
COMPLETE and TIMELY (L-5, L-6), and the fast path must ACTUALLY RUN (L-8).

BOTH GATES ARE RUN FOR EVERY MUTATION, and both verdicts are reported:

  lock    tests/test_resample_lock_gate.c -- the predicate's soundness, its
          non-vacuity, and the two downstream readers (the FX rack's time index
          and the per-stem meter's reported position).
  diff    tests/test_rs_cursor_gate.c -- 200,000 randomized specs, half of them
          locked-shaped, against a frozen transcription of the PRE-EXTRACTION
          main.c text. This is the bit-identity arm.

The contract is that the LOCK gate goes red for all eight. The diff column is
reported because it says something different and worth knowing: which of these
mutations change the AUDIO (diff red) versus which only corrupt state that no
sample carries (diff green, lock red) -- L-6 is the interesting one, because a
missing broadcast is inaudible in the mix and still wrong.

Every mutation is applied to the PRODUCTION header, src/st_rs_cursor.h -- the
same file main.c includes. Every mutation must still COMPILE.

Usage:  stemtape_player_resample_lock_mutations.py [<out-report.md>]
Run from the repository root. Exits non-zero on any hole.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile

SRC = "firmware/stemtape_player/src"
HDR = f"{SRC}/st_rs_cursor.h"
LOCK_GATE = "firmware/stemtape_player/tests/test_resample_lock_gate.c"
DIFF_GATE = "firmware/stemtape_player/tests/test_rs_cursor_gate.c"
DEPS = [f"{SRC}/st_planar.c", f"{SRC}/st_pitch.c", f"{SRC}/st_sector_v11.c"]

PREDICATE = """	if (dirs[0] <= 0) {
		return false;
	}
	for (sp = 1; sp < ST_PL_STEMS; sp++) {
		if (dirs[sp] <= 0 ||
		    frame_in_group[sp] != frame_in_group[0] ||
		    src_avail[sp]      != src_avail[0]      ||
		    frac[sp]           != frac[0]           ||
		    prev_valid[sp]     != prev_valid[0]) {
			return false;
		}
	}
	return true;"""

BROADCAST = """		for (sp = 0; sp < ST_PL_STEMS; sp++) {
			frac[sp] = f0;
			cur[sp]  = c0;
		}
		return;"""


# EVERY MUTANT MUST COMPILE. Dropping a condition orphans the parameter it
# read, and -Wunused-parameter -Werror would then turn the mutation into a
# BUILD failure -- which proves nothing about the gate. This repository has
# already been bitten by a mutation run that "caught" a defect and had really
# just failed to compile, so each dropped condition takes an explicit (void)
# with it.
PARAM_OF = {
    "dirs": "dirs",
    "fig": "frame_in_group",
    "avail": "src_avail",
    "frac": "frac",
    "valid": "prev_valid",
}


def pred_without(*drop: str) -> str:
    """The predicate with one or more conditions removed, still compiling."""
    conds = {
        "dirs": "dirs[sp] <= 0 ||",
        "fig": "frame_in_group[sp] != frame_in_group[0] ||",
        "avail": "src_avail[sp]      != src_avail[0]      ||",
        "frac": "frac[sp]           != frac[0]           ||",
        "valid": "prev_valid[sp]     != prev_valid[0]",
    }
    voids = "".join("\t(void)%s;\n" % PARAM_OF[d] for d in drop)
    keep = [v for k, v in conds.items() if k not in drop]
    # The last surviving condition must not end in ||.
    keep = [c.rstrip().rstrip("|").rstrip() for c in keep]
    body = "\n".join("\t\t    " + c + (" ||" if i < len(keep) - 1 else "")
                     for i, c in enumerate(keep))
    head = "" if "dirs" in drop else "\tif (dirs[0] <= 0) {\n\t\treturn false;\n\t}\n"
    if not keep:
        return voids + head + "\t(void)sp;\n\treturn true;"
    return (voids + head +
            "\tfor (sp = 1; sp < ST_PL_STEMS; sp++) {\n"
            "\t\tif (" + body.lstrip() + ") {\n"
            "\t\t\treturn false;\n"
            "\t\t}\n"
            "\t}\n"
            "\treturn true;")


ALL_VOID = ("\t(void)frame_in_group;\n\t(void)dirs;\n\t(void)src_avail;\n"
            "\t(void)frac;\n\t(void)prev_valid;\n\t(void)sp;\n")


MUTATIONS = [
    (
        "L-1 the predicate stops requiring an equal carried fraction",
        PREDICATE, pred_without("frac"),
        "THE st63 STATE SHIPS BROKEN. main.c's reverse-release clears ONLY the "
        "rejoining lane's s_stem_rate_frac[j] and seeks it to MASTER, so one "
        "block later all four are co-located and forward -- `together` is true "
        "-- while that lane carries fraction 0 and three carry a fraction. "
        "Sharing renders it at the other three's sub-sample phase: audible "
        "only off centre pitch, only just after a reverse release.",
    ),
    (
        "L-2 the predicate stops requiring an equal run bound",
        PREDICATE, pred_without("avail"),
        "A starved lane borrows the transport's offset and a forward direction, "
        "so `together` stays true while its run bound differs -- and the bound "
        "is what the floored corner of st_rs_out_frames() clamps against. "
        "Sharing lane 0's bound lets a short lane be advanced past source it "
        "never had resident.",
    ),
    (
        "L-3 the predicate stops requiring equal interpolator validity",
        PREDICATE, pred_without("valid"),
        "The shared prime reads lane 0's flag. With the flags unequal it either "
        "leaves the rejoining lane interpolating from audio at a position it no "
        "longer occupies, or re-primes three lanes that never left -- "
        "flattening their first output frame.",
    ),
    (
        "L-4 the predicate degenerates to `together`",
        PREDICATE, pred_without("dirs", "fig"),
        "Direction and position are the two conditions `together` DOES check, "
        "and dropping them is the crudest possible break: a reversed lane, or "
        "a lane at a different frame, would be rendered at lane 0's index and "
        "in lane 0's direction. This is per-track reverse silently destroyed.",
    ),
    (
        "L-5 the walk broadcasts the cursor but not the fraction",
        BROADCAST,
        "		for (sp = 0; sp < ST_PL_STEMS; sp++) {\n"
        "			cur[sp]  = c0;\n"
        "		}\n"
        "		return;",
        "The most plausible slip in the whole change: publish the obvious piece "
        "of state and forget the other. Lanes 1-3 keep the fraction they "
        "entered the run with, so the blend weights drift apart frame by frame "
        "and frac_io[] carries the error into the next run.",
    ),
    (
        "L-6 the walk does not broadcast at all",
        BROADCAST, "		return;",
        "THE NAMED HAZARD. Lanes 1-3 keep the PREVIOUS output frame's cursor. "
        "The mix is unaffected -- the blend reads prev/nxt, not cur -- so this "
        "is INAUDIBLE, and still wrong: st_fx_process() is handed the wrong "
        "time index for the echo, g_stem_zero_at[sp] reports a dropout at a "
        "frame it did not happen at, and used_out[] advances three playheads "
        "by a distance they did not travel.",
    ),
    (
        "L-7 the predicate always says locked",
        PREDICATE, ALL_VOID + "\treturn true;",
        "Every divergence -- reverse, a displaced lane, a starved lane, the "
        "st63 release -- takes the shared path. This is the change with its "
        "safety condition deleted, which is the thing the predicate exists to "
        "be.",
    ),
    (
        "L-8 the predicate never says locked",
        PREDICATE, ALL_VOID + "\treturn false;",
        "The optimisation becomes DEAD CODE. Note what this mutation does NOT "
        "break: the audio is still bit-identical, the mutation costs nothing, "
        "and every soundness case above still passes. Only the non-vacuity "
        "case can catch it -- which is precisely why that case exists, and why "
        "a gate without it would have let a no-op ship as a fix.",
    ),
]


def build_and_run(root: str, gate: str) -> tuple[bool, str]:
    """Returns (passed, detail). A build failure is NOT a pass."""
    binp = os.path.join(root, "gate_" + os.path.basename(gate))
    cc = subprocess.run(
        ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
         "-I" + os.path.join(root, SRC)]
        + [os.path.join(root, d) for d in DEPS]
        + [os.path.join(root, gate), "-o", binp],
        capture_output=True, text=True)
    if cc.returncode != 0:
        return False, "did not compile: " + cc.stderr.strip().split("\n")[0]
    run = subprocess.run([binp], capture_output=True, text=True, cwd=root)
    tail = [ln for ln in run.stdout.strip().split("\n") if ln]
    detail = next((ln for ln in reversed(tail) if "failures" in ln),
                  tail[-1] if tail else "<no output>")
    return run.returncode == 0, detail


def main() -> int:
    report = ["# Stem Tape shared-lane resampler cursor -- mutation coverage", ""]
    repo = os.getcwd()

    with tempfile.TemporaryDirectory() as base:
        clean = os.path.join(base, "clean")
        shutil.copytree(os.path.join(repo, "firmware"),
                        os.path.join(clean, "firmware"))

        for gate, name in ((LOCK_GATE, "lock"), (DIFF_GATE, "diff")):
            ok, detail = build_and_run(clean, gate)
            if not ok:
                print(f"FATAL: the UNMUTATED {name} gate does not pass: {detail}",
                      file=sys.stderr)
                return 1
            report.append(f"Baseline, {name} gate: **passes** -- `{detail}`")
        report.append("")
        report.append("Every mutation is applied to the PRODUCTION header "
                      "`src/st_rs_cursor.h`. The contract is that the **lock** "
                      "gate goes red for all eight; the **diff** column says "
                      "whether the mutation also changes the audio.")
        report.append("")
        report.append("| # | mutation | lock gate | diff gate | what it would ship |")
        report.append("|---|---|---|---|---|")

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

            lock_pass, lock_detail = build_and_run(work, LOCK_GATE)
            diff_pass, diff_detail = build_and_run(work, DIFF_GATE)
            if lock_pass:
                holes += 1
                lock_v = "**STILL PASSES -- HOLE**"
            else:
                lock_v = f"red ({lock_detail})"
            diff_v = "green (audio unchanged)" if diff_pass else f"red ({diff_detail})"
            print(f"{'HOLE' if lock_pass else 'ok  '}  {name}\n"
                  f"        lock: {lock_v}\n        diff: {diff_v}")
            report.append(f"| {name.split()[0]} | {' '.join(name.split()[1:])} "
                          f"| {lock_v} | {diff_v} | {why} |")

    report.append("")
    if holes:
        report.append(f"**{holes} mutation(s) survived the lock gate.** It does "
                      f"not actually test what it claims.")
    else:
        report.append(f"All {len(MUTATIONS)} mutations turn the lock gate red, "
                      f"and all of them mutate PRODUCTION code rather than a "
                      f"model of it.")
        report.append("")
        report.append("Read the two columns together. A mutation that is red in "
                      "**diff** changed the audio. One that is red in **lock** "
                      "and green in **diff** changed something no sample "
                      "carries -- the FX rack's time index, the meter's "
                      "reported position, a playhead's advance -- which is "
                      "exactly the class of defect a sample hash cannot see, "
                      "and the reason the lock gate models those two readers.")
        report.append("")
        report.append("What none of it proves: that main.c computes the "
                      "predicate and passes it to all five helpers. That is the "
                      "wiring check (I-1/I-3), which reads production main.c "
                      "directly.")

    if len(sys.argv) > 1:
        with open(sys.argv[1], "w", encoding="utf-8") as fh:
            fh.write("\n".join(report) + "\n")
    print()
    print("\n".join(report))
    return 1 if holes else 0


if __name__ == "__main__":
    sys.exit(main())
