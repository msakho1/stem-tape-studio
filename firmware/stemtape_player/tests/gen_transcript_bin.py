#!/usr/bin/env python3
"""Converts a handoff/v1.1/transcripts/*.json wire transcript into a raw
binary sidecar the C replay harness (test_stem_v11_transcripts.c) reads at
runtime via fopen() -- the SAME pattern test_stem_v11.c's read_fixture()
already uses for every other handoff/v1.1/ fixture, extended here because
the transcripts are JSON (hex strings), not already-raw binary.

This script does NOT interpret or decide anything about the wire protocol
-- it is a mechanical hex-decode + concatenation, not a reimplementation of
any firmware logic. The C harness is what calls the real st_ab_session.c/
st_stix.c/st_stcp.c functions and makes every accept/reject/selection
decision; this script only gets the real recorded bytes from JSON text
into a form C can fopen() and parse without a JSON parser.

Verifies the source transcript's SHA-256 against handoff/v1.1/SHA256SUMS.txt
before emitting anything -- hard failure (nonzero exit, no output file) on
any mismatch or missing manifest entry, so a corrupted or substituted
transcript can never silently reach the replay harness. This is the
"verify every transcript's SHA-256 before replay" step, run immediately
before compilation in CI (see .github/workflows/firmware.yml's own call
site) -- as close to "before replay" as a static host-test binary can get.

Output format (a trivial length-prefixed record stream, magic "TXNP"):
    [4 bytes]  magic "TXNP"
    [4 bytes LE]  entry_count
    per entry:
      [1 byte]  dir: 0 = tx (host -> device), 1 = rx (device -> host)
      [4 bytes LE]  payload length
      [length bytes]  payload (the real recorded wire bytes, hex-decoded verbatim)

Usage: gen_transcript_bin.py <repo-root> <transcript-name> <out.bin>
  <transcript-name> is the JSON's basename without .json, e.g.
  "upload-1-successful" -- resolved to
  <repo-root>/handoff/v1.1/transcripts/<transcript-name>.json and checked
  against the matching line in <repo-root>/handoff/v1.1/SHA256SUMS.txt.
"""
from __future__ import annotations

import hashlib
import json
import struct
import sys


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    repo_root, name, out_path = sys.argv[1], sys.argv[2], sys.argv[3]

    src_path = f"{repo_root}/handoff/v1.1/transcripts/{name}.json"
    manifest_path = f"{repo_root}/handoff/v1.1/SHA256SUMS.txt"
    rel_path = f"handoff/v1.1/transcripts/{name}.json"

    try:
        with open(src_path, "rb") as f:
            raw = f.read()
    except OSError as e:
        print(f"FATAL: could not read transcript {src_path}: {e}", file=sys.stderr)
        return 1

    actual_sha256 = hashlib.sha256(raw).hexdigest()

    expected_sha256 = None
    try:
        with open(manifest_path, "r") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                digest, _, manifest_rel = line.partition("  ")
                if manifest_rel == rel_path:
                    expected_sha256 = digest
                    break
    except OSError as e:
        print(f"FATAL: could not read manifest {manifest_path}: {e}", file=sys.stderr)
        return 1

    if expected_sha256 is None:
        print(f"FATAL: {rel_path} has no entry in {manifest_path} -- refusing to trust an "
              f"unmanifested transcript", file=sys.stderr)
        return 1
    if actual_sha256 != expected_sha256:
        print(f"FATAL: {rel_path} SHA-256 mismatch -- manifest says {expected_sha256}, "
              f"actual file is {actual_sha256}. Refusing to emit a replay sidecar for a "
              f"transcript that does not match its frozen manifest.", file=sys.stderr)
        return 1

    doc = json.loads(raw)
    entries = doc["entries"]

    out = bytearray()
    out += b"TXNP"
    out += struct.pack("<I", len(entries))
    for e in entries:
        dir_byte = 0 if e["dir"] == "tx" else 1
        payload = bytes.fromhex(e["hex"])
        out += bytes([dir_byte])
        out += struct.pack("<I", len(payload))
        out += payload

    with open(out_path, "wb") as f:
        f.write(out)

    print(f"OK: {rel_path} SHA-256 verified ({actual_sha256}), {len(entries)} entries -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
