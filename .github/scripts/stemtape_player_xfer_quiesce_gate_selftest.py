#!/usr/bin/env python3
"""
Prove the quiesce gate is not vacuous: every check must FAIL when violated.

A structural gate that greps for text it can always find is worse than no
gate, because it reports PASS while proving nothing. So each of the gate's
assertions gets a mutant that breaks exactly what it claims to protect, and
the gate is required to reject it.

The mutants are the real failure modes, not arbitrary edits:

  N1   a flag is deleted outright
  N2   the audio thread silences without acknowledging
  N3   the streamer skips its pass without acknowledging
  N4   the acknowledgements are never cleared -- a stale ack from the
       previous transfer stands in for a fresh one
  N4b  they are cleared, but AFTER transfer mode is entered, which is the
       same race wearing the right lines of code
  N5   dispatch is not gated at all
  N5b  the gate settles for ONE acknowledgement instead of both
  N5c  the gate is real, correct, and sits AFTER the command byte is
       consumed -- the subtlest one, and the reason the gate checks
       positions rather than mere presence
  N6   the gate has no escape, so a thread that never acknowledges strands
       the device in a transfer mode that does nothing

N5c earned its place: an earlier version of this self-test appeared to show
the gate missing it, and the fault was in the mutant (its anchor also
matched a more deeply indented copy of the same line earlier in the file,
so it inserted the gate BEFORE the command read rather than after). Worth
recording, because a mutant that does not mutate what it claims to is the
one way a self-test can lie in the reassuring direction.

Usage: stemtape_player_xfer_quiesce_gate_selftest.py [SRC] [GATE] [REPORT]
"""
import os
import subprocess
import sys
import tempfile

DEFAULT_SRC = "firmware/stemtape_player/src/main.c"
DEFAULT_GATE = ".github/scripts/stemtape_player_xfer_quiesce_gate.py"

AUDIO_ACK = "\t\tatomic_set(&g_xfer_audio_quiesced, 1);\n"
STREAM_ACK = "\t\t\tatomic_set(&g_xfer_stream_quiesced, 1);\n"
CLEAR = ("\t\t\t\tatomic_set(&g_xfer_audio_quiesced, 0);\n"
         "\t\t\t\tatomic_set(&g_xfer_stream_quiesced, 0);\n")
DROP = "\t\t\t\tstem_loop_pins_drop();\n"
ENTER = "\t\t\t\tg_xfer_mode = 1;\n"
SCRATCH = "\treturn g_stem_loop_pin_bufs[ST_XFER_SCRATCH_PIN];\n"
HELPER = ("\treturn atomic_get(&g_xfer_audio_quiesced) != 0 &&\n"
          "\t       atomic_get(&g_xfer_stream_quiesced) != 0;\n")
ESCAPE = ("\t\tif (k_uptime_get() - last > 1000) {\n"
          "\t\t\tg_slot_switch_req = 1;\n"
          "\t\t\tg_xfer_mode = 0;\n"
          "\t\t}\n")


def sub(text, old, new, count=1):
    if old not in text:
        raise LookupError("mutation anchor not found: " + repr(old[:60]))
    return text.replace(old, new, count)


def relocate_gate(text):
    """Cut the gate block out of its position, leaving nothing behind."""
    i = text.index("\tif (!xfer_quiesced()) {")
    end = "\n\t\treturn;\n\t}\n"
    j = text.index(end, i) + len(end)
    return text[:i] + text[j:]


MUTANTS = [
    ("N1  the stream flag is deleted",
     lambda s: sub(s, "static atomic_t g_xfer_stream_quiesced;\n", "")),

    ("N2  the audio thread silences WITHOUT acknowledging",
     lambda s: sub(s, AUDIO_ACK, "")),

    ("N3  the streamer skips its pass WITHOUT acknowledging",
     lambda s: sub(s, STREAM_ACK, "")),

    ("N4  the acks are never cleared, so a stale one can stand in",
     lambda s: sub(s, CLEAR, "")),

    # Two steps, because a comment block sits between the clears and the drop:
    # cut the clears out, then paste them back below the entry.
    ("N4b the acks are cleared AFTER transfer mode is entered",
     lambda s: sub(sub(s, CLEAR, ""), ENTER, ENTER + CLEAR)),

    ("N5  dispatch is not gated at all",
     lambda s: sub(s, "\tif (!xfer_quiesced()) {", "\tif (0) {")),

    ("N5b xfer_quiesced() settles for ONE acknowledgement",
     lambda s: sub(s, HELPER,
                   "\treturn atomic_get(&g_xfer_audio_quiesced) != 0;\n")),

    ("N5c the gate is real but sits AFTER the command byte is consumed",
     lambda s: sub(relocate_gate(s),
                   "\n\tlast = k_uptime_get();\n",
                   "\n\tlast = k_uptime_get();\n"
                   "\tif (!xfer_quiesced()) {\n" + ESCAPE +
                   "\t\treturn;\n"
                   "\t}\n")),

    ("N6  the gate has no escape and can strand the device",
     lambda s: sub(s, ESCAPE, "")),

    ("N7  the verify scratch gets its own 8 KB allocation back",
     lambda s: sub(s, SCRATCH, "\treturn s_v11_verify_scratch;\n").replace(
         "static uint8_t g_stem_loop_pin_bufs",
         "static uint8_t s_v11_verify_scratch[ST11_SECTOR_BYTES];\n"
         "static uint8_t g_stem_loop_pin_bufs", 1)),

    ("N8  the scratch aliases the READ-AHEAD RING instead of a pin",
     lambda s: sub(s, SCRATCH, "\treturn g_stem_sector_bufs[0];\n")),

    ("N9  the pins are NOT dropped before transfer mode is entered",
     lambda s: sub(s, DROP, "")),

    ("N9b the pins are dropped AFTER transfer mode is entered",
     lambda s: sub(s, DROP + ENTER, ENTER + DROP)),
]


def run(gate, path):
    p = subprocess.run([sys.executable, gate, path],
                       capture_output=True, text=True)
    out = (p.stdout + p.stderr).strip().splitlines()
    return p.returncode, (out[0] if out else "(no output)")


def main():
    src_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    gate = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_GATE
    report = sys.argv[3] if len(sys.argv) > 3 else None

    src = open(src_path, encoding="utf-8").read()
    lines = []

    rc, line = run(gate, src_path)
    lines.append(("PASS" if rc == 0 else "BROKEN") + " baseline: " + line)
    if rc != 0:
        print("\n".join(lines))
        return 1

    bad = 0
    for name, mut in MUTANTS:
        try:
            text = mut(src)
        except (LookupError, ValueError) as e:
            lines.append("BROKEN   " + name + " -- " + str(e))
            bad += 1
            continue
        if text == src:
            lines.append("BROKEN   " + name + " -- the mutation changed nothing")
            bad += 1
            continue
        fd, tmp = tempfile.mkstemp(suffix=".c")
        os.close(fd)
        try:
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(text)
            rc, line = run(gate, tmp)
        finally:
            os.unlink(tmp)
        if rc == 0:
            lines.append("SURVIVED " + name + "  <-- the gate does not catch this")
            bad += 1
        else:
            lines.append("caught   " + name)
            lines.append("           " + line)

    lines.append("")
    lines.append("ALL %d MUTANTS CAUGHT" % len(MUTANTS) if bad == 0
                 else "%d PROBLEM(S)" % bad)
    print("\n".join(lines))

    if report:
        with open(report, "w", encoding="utf-8") as f:
            f.write("### Transfer-mode quiesce gate self-test\n\n")
            f.write("Each of the gate's checks, broken on purpose. "
                    "The gate must reject every one:\n\n```text\n")
            f.write("\n".join(lines) + "\n```\n")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
