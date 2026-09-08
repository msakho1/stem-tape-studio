#!/usr/bin/env python3
"""
stemtape_player_reverse_start_mutations.py -- prove
tests/test_reverse_start_gate.c actually fails when the START_OF_SONG parking
rules are broken.

WHY. A green gate that cannot go red is worth nothing, and this repository has
been bitten by exactly that: E-2 printed "runs unconditionally" while checking
three call orderings, and a mutation run that "caught" a defect had really just
failed to compile. Each mutation below removes ONE property from the gate's own
model of stem_audio_block(), rebuilds, and requires the gate to FAIL.

THE THREE PROPERTIES THE FIX IS MADE OF, and one mutation each:

  S-1  START_OF_SONG may not bound the source run
  S-2  START_OF_SONG may not trip the co-location underrun guard
  S-3  a parked head renders SILENCE, not repeated frame 0

They are mutated at their three separate CONSUMERS rather than at the single
production line that feeds all three, so each is shown to be independently
observable. S-4 reverts the production line itself, which is the whole fix at
once.

A FINDING FROM THIS RUN, recorded rather than quietly dropped. A sixth
mutation was written first: remove ST_STREAM_START_OF_SONG from the terminal
test at the top of st_stream_advance_frames(). It survived, and correctly so --
for a BACKWARD head the clamp further down re-parks it at 0 within the same
call, so the state test is not independently observable there. What actually
holds a reversed head at the front of the song is the CLAMP, which is what S-5
now mutates instead.

Every mutation must still COMPILE.

Usage:  stemtape_player_reverse_start_mutations.py [<out-report.md>]
Run from the repository root. Exits non-zero on any hole.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile

GATE = "firmware/stemtape_player/tests/test_reverse_start_gate.c"
SRC = "firmware/stemtape_player/src"
DEPS = [f"{SRC}/st_stem_stream.c"]

RESIDENCY = """			resident[k] = (m->head[k].ready_sector == needed[k]) &&
				      (m->head[k].state != ST_STREAM_START_OF_SONG);"""

MUTATIONS = [
    (
        "S-1 START_OF_SONG is allowed to bound the source run",
        "			if (!resident[k]) {\n"
        "				from_zero[k] = true;   /* the all-zero group */\n"
        "				continue;              /* bounds nothing */\n"
        "			}",
        "			if (!resident[k] &&\n"
        "			    m->head[k].state != ST_STREAM_START_OF_SONG) {\n"
        "				from_zero[k] = true;\n"
        "				continue;\n"
        "			}",
        "THE DEFECT ITSELF. A parked head at frame 0 computes fis = 0, so "
        "rk = fis + 1 = 1 and `run` is pinned to ONE frame -- 256 full "
        "run-loop passes per block instead of one or two, which starved the "
        "streamer, froze the transport through the co-location guard, and "
        "left MAIN unable to decode a PLAY tap or a reverse double-tap.",
    ),
    (
        "S-2 START_OF_SONG is allowed to trip the co-location guard",
        "			if (!resident[k] &&\n"
        "			    m->head[k].state != ST_STREAM_START_OF_SONG &&\n"
        "			    m->head[k].song_frame == tr->song_frame) {",
        "			if (!resident[k] &&\n"
        "			    m->head[k].song_frame == tr->song_frame) {",
        "An intentionally independent stem that has merely reached a boundary "
        "stalls the WHOLE MIX whenever the transport happens to share its "
        "frame. This became reachable only because the fix correctly stops "
        "counting a parked head as resident, so it is the fix's own exposure "
        "to close.",
    ),
    (
        "S-3 the parked head renders frame 0 instead of silence",
        "			if (from_zero[k]) {\n"
        "				m->zero_frames[k] += out_n;\n"
        "			} else {\n"
        "				m->real_frames[k] += out_n;\n"
        "			}",
        "			if (from_zero[k] &&\n"
        "			    m->head[k].state != ST_STREAM_START_OF_SONG) {\n"
        "				m->zero_frames[k] += out_n;\n"
        "			} else {\n"
        "				m->real_frames[k] += out_n;\n"
        "			}",
        "`!resident ? silent_group : real` is the ONLY thing that makes a "
        "head silent. A head excluded from the bound but still counted "
        "resident would render its real sector-0 bytes at the TRANSPORT's "
        "offset -- worse than the defect being fixed.",
    ),
    (
        "S-4 the whole fix is reverted at its single production line",
        RESIDENCY,
        "			resident[k] = (m->head[k].ready_sector == needed[k]);",
        "All three consumers regress at once: the parked head bounds the run, "
        "renders frame 0, and is counted as an active reader.",
    ),
    (
        "S-5 the backward clamp at frame 0 is off by one",
        "		if (count > st->song_frame) {\n"
        "			st->song_frame = 0u;\n"
        "			st->state = ST_STREAM_START_OF_SONG;\n"
        "			return ST_STREAM_TICK_START_REACHED;\n"
        "		}",
        "		if (count > st->song_frame + 1u) {\n"
        "			st->song_frame = 0u;\n"
        "			st->state = ST_STREAM_START_OF_SONG;\n"
        "			return ST_STREAM_TICK_START_REACHED;\n"
        "		}",
        "The clamp -- not the state test at the top of the function -- is what "
        "actually stops a backward head at frame 0. Off by one, a head on "
        "frame 0 subtracts 1 and UNDERFLOWS to 0xFFFFFFFF: a playhead an "
        "astronomical distance past the end of the song, on a part with no "
        "MMU. This one mutates the PRODUCTION module, not the model.",
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
    report = ["# Stem Tape reverse start-of-song gate -- mutation coverage", ""]
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
        report.append(f"All {len(MUTATIONS)} mutations turn the gate red. Four "
                      f"mutate the gate's model of stem_audio_block(); S-5 "
                      f"mutates the production st_stem_stream.c.")
        report.append("")
        report.append("This proves the gate DISCRIMINATES. It does not prove "
                      "main.c matches the model it checks -- that is the "
                      "model's stated limitation, and the reason the wiring "
                      "check (H-1/H-2) reads the production file directly.")

    if len(sys.argv) > 1:
        with open(sys.argv[1], "w", encoding="utf-8") as fh:
            fh.write("\n".join(report) + "\n")
    print()
    print("\n".join(report))
    return 1 if holes else 0


if __name__ == "__main__":
    sys.exit(main())
