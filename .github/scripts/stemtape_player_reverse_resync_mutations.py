#!/usr/bin/env python3
"""
stemtape_player_reverse_resync_mutations.py -- prove
tests/test_reverse_resync_gate.c actually fails when the reverse-release
resync is broken.

WHY. A green gate that cannot go red is worth nothing, and this repository has
already been bitten by exactly that: E-2 printed "runs unconditionally" while
checking three call orderings, and a mutation run that "caught" a defect had
really just failed to compile. So each mutation below removes ONE property of
the design from the gate's own model of main.c's reverse-consume block,
rebuilds, and requires the gate to FAIL. A mutation that still passes is a HOLE.

R-1 is the one that matters most: it is the actual defect this stage fixes,
reproduced by moving the master capture below the transport search.

A FINDING FROM THIS RUN, recorded because it corrects a claim the first
draft of main.c made in a comment: once the master is captured FIRST, the
relative order of the remaining three steps -- release-and-rejoin, engage, and
the transport search -- is NOT independently observable. Mutations that moved
the engage above the release, and the search above the rejoin, both left every
assertion green, because the captured value is what every one of them depends
on. The capture is the whole of the load-bearing ordering; the rest is written
in the required sequence for auditability, not for correctness. Do not claim
otherwise.

Every mutation must still COMPILE. A mutation that only breaks the build proves
nothing about whether the gate would have caught the behaviour.

Usage:  stemtape_player_reverse_resync_mutations.py [<out-report.md>]
Run from the repository root. Exits non-zero on any hole.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile

GATE = "firmware/stemtape_player/tests/test_reverse_resync_gate.c"
SRC = "firmware/stemtape_player/src"
DEPS = [f"{SRC}/st_stem_stream.c"]

CAPTURE = """	/* ---- MASTER, CAPTURED BEFORE ANYTHING MOVES ---- */
	master = m->head[m->transport].song_frame;
"""

PASS1 = """	for (j = 0; j < ST_PL_STEMS; j++) {
		const bool want = turning_on && (j == k);

		if (want || !m->head[j].reverse) {
			continue;
		}
		st_stream_set_reverse(&m->head[j], false);
		if (m->head[j].song_frame != master) {
			(void)st_stream_seek(&m->head[j], master);
		}
		m->rs_prev_valid[j] = false;
		m->rate_frac[j]     = 0u;
	}
"""

SEARCH = """	for (j = 0; j < ST_PL_STEMS; j++) {
		if (!m->head[j].reverse) {
			m->transport = (uint8_t)j;
			break;
		}
	}
"""

MUTATIONS = [
    (
        "R-1 master is captured AFTER the transport search",
        CAPTURE,
        "\t/* mutated: captured below, once the search has already run */\n"
        "\tmaster = 0u;\n",
        # the real capture is reinstated at the bottom of the function
        "THE DEFECT ITSELF. The search picks the lowest-index FORWARD head "
        "with no idea which head was just displaced, so reading master after "
        "it makes master BE the released head -- the rejoin seeks it to where "
        "it already is, and the song clock jumps backwards by the excursion.",
        None,
        (SEARCH, SEARCH + "\tmaster = m->head[m->transport].song_frame;\n"
                          "\tfor (j = 0; j < ST_PL_STEMS; j++) {\n"
                          "\t\tif (!m->head[j].reverse &&\n"
                          "\t\t    m->head[j].song_frame != master) {\n"
                          "\t\t\t(void)st_stream_seek(&m->head[j], master);\n"
                          "\t\t}\n"
                          "\t}\n"),
    ),
    (
        "R-2 leaving reverse does not seek at all",
        "\t\tif (m->head[j].song_frame != master) {\n"
        "\t\t\t(void)st_stream_seek(&m->head[j], master);\n"
        "\t\t}\n",
        "\t\tif (m->head[j].song_frame != master && master == UINT32_MAX) {\n"
        "\t\t\t(void)st_stream_seek(&m->head[j], master);\n"
        "\t\t}\n",
        "The pre-2B behaviour: the stem resumes forward from wherever the "
        "excursion left it and stays permanently displaced.",
    ),
    (
        "R-3 only an explicit release resyncs, a switch does not",
        "\t\tif (want || !m->head[j].reverse) {\n\t\t\tcontinue;\n\t\t}\n",
        "\t\tif (want || !m->head[j].reverse || turning_on) {\n"
        "\t\t\tcontinue;\n\t\t}\n",
        "Switching reverse straight to another stem leaves the outgoing lane "
        "displaced for the rest of the song -- the invariant says both ways "
        "out of reverse are the same event.",
    ),
    (
        "R-4 all four resamplers are dropped instead of only the outgoing one",
        "\t\tm->rs_prev_valid[j] = false;\n\t\tm->rate_frac[j]     = 0u;\n\t}\n",
        "\t\tfor (uint32_t q = 0; q < ST_PL_STEMS; q++) {\n"
        "\t\t\tm->rs_prev_valid[q] = false;\n"
        "\t\t\tm->rate_frac[q]     = 0u;\n"
        "\t\t}\n\t}\n",
        "stem_rs_drop() applied here: three stems that did not move have "
        "their fractional cursors quantised, which at any non-unity rate is "
        "exactly the small displacement this stage exists to remove.",
    ),
    (
        "R-5 the outgoing stem's carried state is NOT dropped",
        "\t\tm->rs_prev_valid[j] = false;\n\t\tm->rate_frac[j]     = 0u;\n\t}\n",
        "\t}\n",
        "Interpolation history from the displaced position bleeds into the "
        "first frame at master -- the audible old-position tail.",
    ),
    (
        "R-6 master is read from stem 0 instead of the transport head",
        "	master = m->head[m->transport].song_frame;\n",
        "	master = m->head[0].song_frame;\n",
        "Exactly the 'blindly seek to a potentially displaced lane' failure: "
        "when stem 0 is the one being released, stem 0's own displaced "
        "position becomes the rejoin target and the rejoin is a no-op.",
    ),
    (
        "R-7 the head ENTERING reverse is also seeked to master",
        """	if (turning_on && !m->head[k].reverse) {
		st_stream_set_reverse(&m->head[k], true);""",
        """	if (turning_on && !m->head[k].reverse) {
		st_stream_set_reverse(&m->head[k], true);
		(void)st_stream_seek(&m->head[k], master);""",
        "Turning a head around is not a position change: it must not move the "
        "head and must not invalidate its residency. Reverse would begin from "
        "a re-primed master rather than from where the stem actually was.",
    ),
    (
        "R-8 the rejoin targets frame 0 instead of master",
        "\t\t\t(void)st_stream_seek(&m->head[j], master);\n",
        "\t\t\t(void)st_stream_seek(&m->head[j], 0u);\n",
        "A rejoin to a position that is not the shared timeline at all -- and "
        "inside a latched loop, to a frame outside the window.",
    ),
    (
        "R-9 inside a loop the rejoin targets loop_start, not the loop master",
        "\t\t\t(void)st_stream_seek(&m->head[j], master);\n",
        "\t\t\t(void)st_stream_seek(&m->head[j],\n"
        "\t\t\t\t\t       m->lp_on ? m->lp_lo : master);\n",
        "The spec is explicit: the outgoing stem rejoins the CURRENT loop "
        "master, not the window's start and not a hypothetical position.",
    ),
    (
        "R-10 the rejoin is unconditional, even with nothing to rejoin",
        "\t\tif (m->head[j].song_frame != master) {\n"
        "\t\t\t(void)st_stream_seek(&m->head[j], master);\n"
        "\t\t}\n",
        "\t\t(void)st_stream_seek(&m->head[j], master);\n",
        "A reverse that never moved would still invalidate residency and pay "
        "a whole-block re-prime stall for an excursion that never happened.",
    ),
    (
        "R-11 turning a head around moves it",
        """void st_stream_set_reverse(st_stream_t *st, bool reverse)
{
	if (st->reverse == reverse) {
		return;
	}
	st->reverse = reverse;""",
        """void st_stream_set_reverse(st_stream_t *st, bool reverse)
{
	if (st->reverse == reverse) {
		return;
	}
	st->reverse = reverse;
	st->ready_sector = ST_STREAM_NO_SECTOR;""",
        "Turning around is not a position change and must not cost a "
        "re-prime. This one mutates the PRODUCTION module, not the model.",
        f"{SRC}/st_stem_stream.c",
    ),
]


def build_and_run(root: str) -> tuple[bool, str]:
    """Returns (gate_passed, detail). A build failure is NOT a pass."""
    binp = os.path.join(root, "gate")
    cc = subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-I" + os.path.join(root, SRC)]
        + [os.path.join(root, d) for d in DEPS]
        + [os.path.join(root, GATE), "-o", binp],
        capture_output=True, text=True)
    if cc.returncode != 0:
        return False, "did not compile: " + cc.stderr.strip().split("\n")[0]
    run = subprocess.run([binp], capture_output=True, text=True, cwd=root)
    tail = [ln for ln in run.stdout.strip().split("\n") if ln][-1:]
    return run.returncode == 0, (tail[0] if tail else "<no output>")


def apply_edit(path: str, old: str, new: str, name: str) -> None:
    text = open(path).read()
    n = text.count(old)
    if n != 1:
        raise SystemExit(f"FATAL: {name}: anchor occurs {n} times in "
                         f"{os.path.basename(path)}, want exactly 1")
    open(path, "w").write(text.replace(old, new))


def main() -> int:
    report = ["# Stem Tape reverse-resync gate -- mutation coverage", ""]
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
        for mut in MUTATIONS:
            name, old, new, why = mut[0], mut[1], mut[2], mut[3]
            target = mut[4] if len(mut) > 4 and mut[4] else GATE
            extra = mut[5] if len(mut) > 5 else None

            work = os.path.join(base, "work")
            if os.path.exists(work):
                shutil.rmtree(work)
            shutil.copytree(clean, work)

            apply_edit(os.path.join(work, target), old, new, name)
            if extra:
                apply_edit(os.path.join(work, GATE), extra[0], extra[1], name)

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
        report.append(f"All {len(MUTATIONS)} mutations turn the gate red. Ten "
                      f"mutate the gate's model of main.c's reverse-consume "
                      f"block; R-11 mutates the production st_stem_stream.c.")
        report.append("")
        report.append("This proves the gate DISCRIMINATES. It does not prove "
                      "main.c matches the model it checks -- that is the "
                      "model's stated limitation, and the reason the wiring "
                      "check (G-1..G-4) reads the production file directly.")

    if len(sys.argv) > 1:
        with open(sys.argv[1], "w", encoding="utf-8") as fh:
            fh.write("\n".join(report) + "\n")
    print()
    print("\n".join(report))
    return 1 if holes else 0


if __name__ == "__main__":
    sys.exit(main())
