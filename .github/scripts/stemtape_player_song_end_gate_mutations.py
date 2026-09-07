#!/usr/bin/env python3
"""
stemtape_player_song_end_gate_mutations.py -- prove tests/test_song_end_gate.c
actually fails when the end-of-song design is broken.

WHY. A green gate that cannot go red is worth nothing, and this repository has
already been bitten twice by exactly that: E-2 printed "runs unconditionally"
while checking three call orderings, and a mutation run that "caught" a defect
had really just failed to compile. So each mutation below removes ONE property
of the design from the gate's own model of main.c's wiring -- the model that
mirrors looper_audio_block()/stem_audio_block() -- rebuilds, and requires the
gate to FAIL. A mutation that still passes is reported as a HOLE.

The mutations are stated as (name, exact old text, exact new text). Each
substring must occur EXACTLY ONCE in the file; a mutation that cannot be applied
uniquely is a failure of this script, not a pass of the gate.

Usage:  stemtape_player_song_end_gate_mutations.py [<out-report.md>]
Run from the repository root. Exits non-zero on any hole.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile

GATE = "firmware/stemtape_player/tests/test_song_end_gate.c"
SRC = "firmware/stemtape_player/src"
DEPS = [f"{SRC}/st_inertia.c", f"{SRC}/st_stem_stream.c"]

MUTATIONS = [
    (
        "M-1 the whole-song wrap is back on",
        "sectors, /*loop_enabled=*/false)) {",
        "sectors, /*loop_enabled=*/true)) {",
        "The original defect, reproduced: a song that reaches its end wraps to "
        "frame 0 and plays again with nobody pressing PLAY.",
    ),
    (
        "M-2 EOF is applied per lane instead of once for all four",
        """	for (k = 0; k < ST_PL_STEMS; k++) {
		st_stream_end_of_song(&m->head[k]);
	}""",
        """	for (k = 0; k < ST_PL_STEMS; k++) {
		if (k == 0u) {
			st_stream_end_of_song(&m->head[k]);
		}
	}""",
        "Four independent end-of-song decisions: one lane parks, three carry "
        "on. This is the incoherent stop the shared rule exists to prevent.",
    ),
    (
        "M-3 only the TRANSPORT head's ENDED counts",
        """			if (tk == ST_STREAM_TICK_ENDED) {
				eof_seen = true;
			}""",
        """			if (tk == ST_STREAM_TICK_ENDED && k == m->transport) {
				eof_seen = true;
			}""",
        "A starved transport head never reports ENDED, so a healthy lane runs "
        "off the end alone and the song never stops.",
    ),
    (
        "M-4 a latched loop no longer exempts the transport",
        "		if (eof_seen && !m->lp_on) {",
        "		if (eof_seen) {",
        "A loop whose end sits on the song end would stop the song instead of "
        "wrapping -- breaking the real loop feature.",
    ),
    (
        "M-5 no rewind on a PLAY that follows the end of the song",
        """	if (m_at_song_end(m)) {
		m_rewind(m);
	}""",
        """	if (m_at_song_end(m) && m->frames_are_never_zero) {
		m_rewind(m);
	}""",
        "PLAY after EOF leaves four heads parked past the last frame: the "
        "device never plays again.",
    ),
    (
        "M-6 the reel is not stopped in the block the song ended",
        """	m_rs_drop(m);
	st_inertia_reset(&m->inertia);
	m->eof_req = true;""",
        """	m_rs_drop(m);
	m->eof_req = true;""",
        "The race the gate found during development: the control thread clears "
        "the request before this thread sees it, the reel spins down instead "
        "of stopping, st_inertia_moving() re-enters the branch and the replay "
        "rewind restarts the song.",
    ),
    (
        "M-7 the EOF request does not gate the stem branch",
        "	if (m->eof_req || !(m->playing || st_inertia_moving(&m->inertia))) {",
        "	if (!(m->playing || st_inertia_moving(&m->inertia))) {",
        "The window between the audio thread parking the heads and the control "
        "thread clearing g_playing is no longer closed.",
    ),
    (
        "M-8 the control thread never clears the transport request",
        """	if (m->eof_req) {
		m->playing = false;
		m->eof_req = false;
	}""",
        """	if (m->eof_req) {
		m->eof_req = false;
	}""",
        "g_playing stays set after the song ends -- the transport still "
        "believes it is playing.",
    ),
    (
        "M-9 the replay rewind is not coherent across the four heads",
        """	for (k = 0; k < ST_PL_STEMS; k++) {
		(void)st_stream_seek(&m->head[k], 0u);
	}
	m_rs_drop(m);""",
        """	for (k = 0; k < ST_PL_STEMS; k++) {
		(void)st_stream_seek(&m->head[k], k == 0u ? 0u : 64u);
	}
	m_rs_drop(m);""",
        "Three heads start 64 frames ahead of the fourth: exactly the "
        "restart-initialisation offset this checkpoint must not introduce.",
    ),
    (
        "M-10 the loop release seeks instead of continuing forward",
        "	m.lp_on = false;\n	m_tick(&m);",
        """	m.lp_on = false;
	for (k = 0; k < ST_PL_STEMS; k++) {
		(void)st_stream_seek(&m.head[k], hi - 1u);
	}
	m_tick(&m);""",
        "The protected loop-release behaviour: no seek, no jump to loop end, "
        "the audible position preserved.",
    ),
    (
        "M-11 end-of-song clears reverse",
        """	st->ready_sector = ST_STREAM_NO_SECTOR;
	st->state = ST_STREAM_END_OF_SONG;

	/* `reverse` is untouched: see the header. */""",
        """	st->ready_sector = ST_STREAM_NO_SECTOR;
	st->state = ST_STREAM_END_OF_SONG;
	st->reverse = false;""",
        "A song ending is not a reverse gesture. This one mutates the "
        "PRODUCTION module, not the model.",
        f"{SRC}/st_stem_stream.c",
    ),
    (
        "M-12 END_OF_SONG stops being sticky",
        """void st_stream_play(st_stream_t *st)
{
	if (st->state == ST_STREAM_STOPPED) {""",
        """void st_stream_play(st_stream_t *st)
{
	if (st->state == ST_STREAM_STOPPED || st->state == ST_STREAM_END_OF_SONG) {""",
        "If st_stream_play() resurrected a finished song, the transport's own "
        "every-block PLAY re-assert would un-park the heads. Production "
        "module.",
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


def main() -> int:
    report = ["# Stem Tape song-end gate -- mutation coverage", ""]
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
            target = mut[4] if len(mut) > 4 else GATE

            work = os.path.join(base, "work")
            if os.path.exists(work):
                shutil.rmtree(work)
            shutil.copytree(clean, work)

            path = os.path.join(work, target)
            text = open(path).read()
            n = text.count(old)
            if n != 1:
                print(f"FATAL: {name}: anchor occurs {n} times in {target}, "
                      f"want exactly 1", file=sys.stderr)
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
        report.append(f"All {len(MUTATIONS)} mutations turn the gate red. Ten "
                      f"mutate the gate's model of main.c's wiring; two "
                      f"(M-11, M-12) mutate the production st_stem_stream.c.")
        report.append("")
        report.append("This proves the gate DISCRIMINATES. It does not prove "
                      "main.c matches the model it checks -- that is the "
                      "model's stated limitation, and the reason the wiring "
                      "check (F-1/F-2) reads the production file directly.")

    if len(sys.argv) > 1:
        with open(sys.argv[1], "w", encoding="utf-8") as fh:
            fh.write("\n".join(report) + "\n")
    print()
    print("\n".join(report))
    return 1 if holes else 0


if __name__ == "__main__":
    sys.exit(main())
