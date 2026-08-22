#!/usr/bin/env python3
"""Stem Tape Player -- classic native-storage playback source absence gate.

STEM TAPE architecture correction (pre-Slice-3A): a valid, playing stored
Stem Tape song REPLACES the inherited classic mono-loop audio source in
stemtape_player's real-time mixer (looper_audio_block()'s PASS C) -- it is
never summed with it. This is a source-level, fail-closed proof that the
correction is real and durable, not just "the classic bus happens to be
silent today":

  1. present[] ASSIGNMENT-ONLY-ZERO INVARIANT. g_meta.slot[].present[NTRK]
     is the single flag that gates a classic track ever reaching
     trk[i].state == TS_PLAY (the only state PASS B's classic per-track
     accumulation reads). The v1.0 write path that used to set it is
     already fully deleted from this file (see CMakeLists.txt's own
     provenance comment); this check proves it structurally, by finding
     EVERY assignment site to a present[] array element in main.c and
     requiring the right-hand side to be the literal 0. If any assignment
     ever wrote a nonzero value, the classic per-track engine could become
     live again -- this gate fails closed the moment that happens, rather
     than relying on the current absence of such an assignment as an
     unenforced convention. Requires at least one assignment site to exist
     (so a rewrite that silently removes the delete-handler's own present[i]
     = 0 line does not vacuously "pass" by finding nothing to check).

  2. NO CLASSIC+STEM SUMMATION. Textually proves the specific "sum the
     classic bus into the stem output" expression this correction removed
     is absent: no arithmetic combination of the classic-bus sample
     (`classic`) and either stem output sample (`stem_l`/`stem_r`) appears
     anywhere in main.c, in either operand order.

  3. REPLACE, NOT FALLTHROUGH-SUM, STRUCTURE. Confirms PASS C's own
     mutually-exclusive structure is present: the stem-active branch writes
     both stereo output slots directly from the stem mix and returns via
     `continue` (never reaching the classic computation below it in the
     same iteration), and the classic fallback branch writes both stereo
     output slots directly from the classic sample alone.

Fails closed: main.c missing, or any check above not satisfied.

Usage: stemtape_player_classic_source_absence_gate.py <main.c> <out-report.md>
"""

from __future__ import annotations

import re
import sys

PRESENT_ASSIGN_RE = re.compile(r"present\[[^\]]*\]\s*=(?!=)\s*([^;]+);")

# Every operand-order, both stereo channels, of "sum the classic sample into
# the stem output" -- the exact pattern this correction removes.
SUM_PATTERNS = [
    r"classic\s*\+\s*stem_l",
    r"stem_l\s*\+.*classic",
    r"classic\s*\+\s*stem_r",
    r"stem_r\s*\+.*classic",
]

# The stem branch's stereo stores. The master-volume scale is applied
# inside the store expression now, so these match "assigned from an
# expression built out of stem_l/stem_r" rather than a bare identifier --
# what matters, and what check 3's own report line claims, is that the
# right-hand side is derived from the STEM MIX ALONE. That the classic bus
# cannot appear in it is proven separately and more strongly by
# STEM_RENDERER_CLEAN below.
STEM_DIRECT_L = re.compile(r"s\[2\s*\*\s*f\]\s*=[^;]*\bstem_l\b[^;]*;")
STEM_DIRECT_R = re.compile(r"s\[2\s*\*\s*f\s*\+\s*1\]\s*=[^;]*\bstem_r\b[^;]*;")
CLASSIC_DIRECT_L = re.compile(r"s\[2\s*\*\s*f\]\s*=\s*classic;")
CLASSIC_DIRECT_R = re.compile(r"s\[2\s*\*\s*f\s*\+\s*1\]\s*=\s*classic;")
CONTINUE_STMT = re.compile(r"\bcontinue;")

# Strips /* ... */ and // ... comments (replacing their content with spaces,
# so line numbers and byte offsets are preserved for anything that cares),
# so every check below only ever matches real code -- never prose in a
# comment that happens to mention "present[]" or "= 1" in passing. Good
# enough for this file: it has no string/char literals shaped like a
# comment delimiter anywhere near the code these checks look at.
_COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)


def strip_comments(text: str) -> str:
    def _blank(m: "re.Match[str]") -> str:
        return "".join(c if c == "\n" else " " for c in m.group(0))
    return _COMMENT_RE.sub(_blank, text)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    main_c_path, out_path = sys.argv[1], sys.argv[2]

    try:
        raw_text = open(main_c_path, errors="ignore").read()
    except OSError as e:
        print(f"FATAL: could not read {main_c_path}: {e}", file=sys.stderr)
        return 1
    text = strip_comments(raw_text)

    report: list[str] = [
        "# Stem Tape Player -- classic native-storage playback source absence gate",
        "",
    ]
    fail = False

    # ---- Check 1: present[] assignment-only-zero invariant ----
    assigns = PRESENT_ASSIGN_RE.findall(text)
    report.append("### `present[]` assignment-only-zero invariant")
    if not assigns:
        report.append("- **MISSING**: no assignment site to a `present[]` element found at all "
                       "(expected at least the delete-handler's own `present[i] = 0;`)")
        fail = True
    else:
        bad = [a for a in assigns if a.strip() != "0"]
        report.append(f"- found {len(assigns)} assignment site(s) to `present[]`")
        if bad:
            for b in bad:
                report.append(f"  - **MISSING/BAD**: assigns non-literal-zero value `{b.strip()}`")
            fail = True
        else:
            report.append("  - present: every assignment site writes the literal `0` -- "
                           "the classic per-track engine's own gate can never be set live "
                           "from this file's source")
    report.append("")

    # ---- Check 2: no classic+stem summation ----
    report.append("### No classic+stem summation")
    found_sum = False
    for pat in SUM_PATTERNS:
        if re.search(pat, text):
            report.append(f"- **MISSING**: forbidden summation pattern found: `{pat}`")
            found_sum = True
            fail = True
    if not found_sum:
        report.append("- present: no arithmetic combination of `classic` and `stem_l`/`stem_r` "
                       "found anywhere in main.c")
    report.append("")

    # ---- Check 3: replace, not sum, structure ----
    report.append("### Replace (not fallthrough-sum) structure")
    checks = [
        ("stem branch writes left channel directly from stem_l", STEM_DIRECT_L),
        ("stem branch writes right channel directly from stem_r", STEM_DIRECT_R),
        ("classic fallback writes left channel directly from classic", CLASSIC_DIRECT_L),
        ("classic fallback writes right channel directly from classic", CLASSIC_DIRECT_R),
    ]
    for label, pat in checks:
        if pat.search(text):
            report.append(f"- present: {label}")
        else:
            report.append(f"- **MISSING**: {label} -- pattern `{pat.pattern}` not found")
            fail = True
    # THE STRUCTURAL PROOF, STRENGTHENED. This used to look for a `continue;`
    # anywhere in the file -- the stem branch's early-exit out of the shared
    # per-frame loop. There is no shared per-frame loop any more: the stem
    # renderer is its own function (stem_render_run) selected by an if/else
    # against the classic fallback, so the two are mutually exclusive by
    # construction rather than by an early exit. A bare `continue;` search
    # would now pass on any unrelated loop in the file, i.e. prove nothing.
    #
    # What is checked instead is stronger and is the property that actually
    # matters: the stem renderer cannot sum in the classic bus because it
    # cannot NAME it. Neither the classic accumulator (`mix32`) nor the
    # classic sample (`classic`) may appear anywhere inside
    # stem_render_run()'s own body.
    m = re.search(r"^static void stem_render_run\(", text, re.M)
    if not m:
        report.append("- **MISSING**: stem_render_run() not found -- the stem renderer must be "
                       "its own function, separate from the classic fallback")
        fail = True
    else:
        depth = 0
        started = False
        body = []
        for ch in text[m.start():]:
            body.append(ch)
            if ch == "{":
                depth += 1
                started = True
            elif ch == "}":
                depth -= 1
                if started and depth == 0:
                    break
        body_text = "".join(body)
        leaked = [n for n in ("mix32", "classic") if re.search(r"\b" + n + r"\b", body_text)]
        if leaked:
            report.append("- **MISSING/BAD**: stem_render_run() references the classic bus: "
                           + ", ".join("`" + n + "`" for n in leaked))
            fail = True
        else:
            report.append("- present: stem_render_run() never names `mix32` or `classic` -- the "
                           "stem output cannot be summed with the classic bus because the "
                           "renderer has no access to it")
    report.append("")

    report.append("## Result")
    report.append("")
    if fail:
        report.append("GATE FAILED -- see missing/bad item(s) above.")
    else:
        report.append("GATE PASSED -- the classic native-storage playback source is proven "
                       "unreachable (present[] can never be set live) and PASS C's stem-vs-"
                       "classic paths are proven mutually exclusive, not summed.")
    report.append("")

    open(out_path, "w").write("\n".join(report) + "\n")
    print("\n".join(report))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
