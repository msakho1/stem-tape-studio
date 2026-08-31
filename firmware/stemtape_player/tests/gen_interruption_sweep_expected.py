#!/usr/bin/env python3
"""Mechanically transcribes handoff/v1.1/reports/interruption-sweep.json's own
146 declared (op, when, active, activeGeneration) results into a small binary
sidecar the C replay harness (test_stem_v11_transcripts.c) reads at runtime
via fopen() -- the same fopen()-a-sidecar pattern gen_transcript_bin.py
already uses for the wire transcripts.

This script makes NO decision of its own and does not touch any firmware
logic. It is a straight field-by-field transcription of the real fixture's
own declared "results" array (already verified, before this script was
written, to be in strict op-ascending, before-then-after order -- see the
citation in test_interruption_sweep_matches_fixture()'s own doc comment) so
that hand-typing 146 rows into C source is never required and no transcript
error can silently creep in between the JSON and the C assertions.

handoff/v1.1/reports/*.json files (unlike handoff/v1.1/binaries/,
handoff/v1.1/decoded/ and handoff/v1.1/transcripts/) are NOT covered by
handoff/v1.1/SHA256SUMS.txt or CRC32SUMS.txt -- confirmed directly by
grepping the manifest before this script was written -- so there is no
manifest entry to verify this file against, unlike gen_transcript_bin.py's
transcripts. This matches how this whole suite already treats the other
unmanifested report (magic-write-cases.json): cited directly, not routed
through a manifest check that does not exist for it.

Output format (magic "ISWP"):
    [4 bytes]  magic "ISWP"
    [4 bytes LE]  record_count (== interruptionSweep.json's own
                  injectedInterruptionPoints, expected 146)
    per record (10 bytes):
      [4 bytes LE]  op        (1-based protocol operation index)
      [1 byte]      when      (0 = "before", 1 = "after")
      [1 byte]      is_new    (0 = active=="previous", 1 = active=="new")
      [4 bytes LE]  generation (the declared activeGeneration)

Usage: gen_interruption_sweep_expected.py <repo-root> <out.bin>
"""
from __future__ import annotations

import json
import struct
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    repo_root, out_path = sys.argv[1], sys.argv[2]
    src_path = f"{repo_root}/handoff/v1.1/reports/interruption-sweep.json"

    try:
        with open(src_path, "r") as f:
            doc = json.load(f)
    except OSError as e:
        print(f"FATAL: could not read {src_path}: {e}", file=sys.stderr)
        return 1

    results = doc["results"]

    out = bytearray()
    out += b"ISWP"
    out += struct.pack("<I", len(results))
    prev_op = 0
    expect_when = "before"
    for r in results:
        op = int(r["op"])
        when = r["when"]
        active = r["active"]
        generation = int(r["activeGeneration"])

        # Sanity-check the strict op-ascending, before-then-after ordering
        # this script relies on (already verified independently before
        # writing it) -- fail closed rather than silently transcribing a
        # differently-shaped file.
        if when != expect_when:
            print(f"FATAL: {src_path}: expected '{expect_when}' at op {op}, got '{when}'", file=sys.stderr)
            return 1
        if when == "before":
            if op != prev_op + 1:
                print(f"FATAL: {src_path}: expected op {prev_op + 1}, got {op}", file=sys.stderr)
                return 1
            prev_op = op
            expect_when = "after"
        else:
            if op != prev_op:
                print(f"FATAL: {src_path}: 'after' op {op} does not match preceding 'before' op {prev_op}",
                      file=sys.stderr)
                return 1
            expect_when = "before"
        if active not in ("previous", "new"):
            print(f"FATAL: {src_path}: unrecognized active value {active!r} at op {op}", file=sys.stderr)
            return 1

        out += struct.pack("<IBBI", op, 0 if when == "before" else 1, 1 if active == "new" else 0, generation)

    with open(out_path, "wb") as f:
        f.write(out)

    print(f"OK: {src_path} -- {len(results)} results transcribed -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
