#!/usr/bin/env python3
"""Self-test for stemtape_player_safety_gate.py's function-name parser.

Proves extract_function_name()/index_functions() (the machinery that
decides which C function a given `emmc_write_blocks()`/`meta_write_blocks()`
call site is inside, for pass D's exact-match against ALLOWED_WRITE_FUNCS)
correctly reads the REAL identifier out of every declaration shape this
codebase uses or might plausibly use -- and never silently mangles a name
the way a real CI run once caught (`static int __attribute__((noinline,
noclone))\\nxfer_songdata_write(...)` was misread as "fer_songdata_write",
missing its leading "x").

No toolchain and no built firmware are needed: every fixture below is a
small, hand-written C-shaped text snippet, exactly like
m0_assertions_selftest.py's own convention for this repo's other
assertion/gate scripts.

Exit code 0 = the parser reads every required shape correctly AND rejects
every lookalike/unauthorized name exactly as before.
"""

from __future__ import annotations

import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GATE_PATH = os.path.join(HERE, "stemtape_player_safety_gate.py")

_spec = importlib.util.spec_from_file_location("stemtape_player_safety_gate", GATE_PATH)
gate = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gate)  # module-level code only defines constants/functions
                                 # (main() is never called at import time) -- see the
                                 # gate script's own module docstring.

extract_function_name = gate.extract_function_name
index_functions = gate.index_functions
find_call_sites = gate.find_call_sites
function_body_bounds = gate.function_body_bounds
ALLOWED_WRITE_FUNCS = gate.ALLOWED_WRITE_FUNCS
TARGET_SYMBOLS = gate.TARGET_SYMBOLS

results: list[tuple[bool, str]] = []


def check(ok: bool, label: str):
    results.append((ok, label))
    print(f"[{'OK  ' if ok else 'FAIL'}] {label}")


# ============================================================================
# Part 1: extract_function_name() on every required declaration shape.
# Every case asserts the COMPLETE, EXACT function name -- not a substring,
# not a prefix/suffix match, not "contains".
# ============================================================================

SHAPES = [
    ("plain single-line",
     "static int xfer_songdata_write(uint32_t sector, const uint8_t data[8192], void *ctx)",
     "xfer_songdata_write"),
    ("attribute BEFORE the return type, same line",
     "static __attribute__((noinline, noclone)) int xfer_songdata_write(uint32_t sector, const uint8_t data[8192], void *ctx)",
     "xfer_songdata_write"),
    ("attribute BETWEEN the return type and the name, same line",
     "static int __attribute__((noinline, noclone)) xfer_songdata_write(uint32_t sector, const uint8_t data[8192], void *ctx)",
     "xfer_songdata_write"),
    ("attribute AFTER the parameter list, same line",
     "static int xfer_songdata_write(uint32_t sector, const uint8_t data[8192], void *ctx) __attribute__((noinline, noclone))",
     "xfer_songdata_write"),
    ("attribute BETWEEN return type and name, split onto its OWN line "
     "(the exact shape a real CI run caught misparsing)",
     "static int __attribute__((noinline, noclone))\nxfer_songdata_write(uint32_t sector, const uint8_t data[8192], void *ctx)",
     "xfer_songdata_write"),
    ("attribute AFTER the parameter list, on its own continuation line "
     "(this codebase's actual current main.c shape)",
     "static int xfer_songdata_write(uint32_t sector, const uint8_t data[8192], void *ctx)\n\t__attribute__((noinline, noclone))",
     "xfer_songdata_write"),
    ("multi-line parameter list, no attribute",
     "static int xfer_songdata_write(uint32_t sector,\n\tconst uint8_t data[8192],\n\tvoid *ctx)",
     "xfer_songdata_write"),
    ("multi-line parameter list WITH a trailing attribute on its own line",
     "static int xfer_songdata_write(uint32_t sector,\n\tconst uint8_t data[8192],\n\tvoid *ctx)\n\t__attribute__((noinline, noclone))",
     "xfer_songdata_write"),
    ("pointer return type",
     "static uint8_t *xfer_songdata_write(uint32_t sector, void *ctx)",
     "xfer_songdata_write"),
    ("pointer-to-const return type",
     "static const uint8_t *xfer_songdata_write(uint32_t sector, void *ctx)",
     "xfer_songdata_write"),
    ("void-argument, no-parameter function",
     "static bool xfer_do_commit(void)",
     "xfer_do_commit"),
    ("void-argument function, attribute between return type and name",
     "static bool __attribute__((noinline, noclone)) xfer_do_commit(void)",
     "xfer_do_commit"),
    ("void-argument function, attribute trailing on its own line",
     "static bool xfer_do_commit(void)\n\t__attribute__((noinline, noclone))",
     "xfer_do_commit"),
    ("a project macro standing in for a return-type-adjacent attribute "
     "(an object-like macro token, no parens of its own in source -- "
     "exactly how e.g. `#define ST_AUDIT_NOINLINE __attribute__((...))` "
     "would appear UNEXPANDED in the raw source this parser reads)",
     "static ST_AUDIT_NOINLINE int xfer_songdata_write(uint32_t sector, const uint8_t data[8192], void *ctx)",
     "xfer_songdata_write"),
    ("xfer_staging_write, the second real adapter",
     "static int xfer_staging_write(uint32_t sector, const uint8_t data[8192], void *ctx)",
     "xfer_staging_write"),
    ("xfer_header_write, the third real adapter",
     "static int xfer_header_write(uint32_t sector, const uint8_t data[8192], void *ctx)",
     "xfer_header_write"),
    ("a non-function initializer must NOT be read as a function at all",
     "static const uint8_t ST_SUBBLOCK_PHYSICAL_ORDER[4] = { 0u, 2u, 1u, 3u }",
     None),
]

for label, text, expect in SHAPES:
    got = extract_function_name(text)
    check(got == expect,
          f"extract_function_name(): {label} -> exact name {expect!r} (got {got!r})")

print()

# ============================================================================
# Part 2: index_functions() + find_call_sites() end to end, reproducing the
# exact real-file shape (forward declaration with a trailing attribute,
# then a plain definition) and proving the emmc_write_blocks() call site
# inside the body is attributed to the COMPLETE, correct name.
# ============================================================================


def build_fixture_lines(sig_line1: str, sig_line2: str | None, body_call: str):
    lines = [sig_line1]
    if sig_line2 is not None:
        lines.append(sig_line2)
    lines += [
        "{",
        "\tARG_UNUSED(ctx);",
        f"\tif ({body_call}) {{ return 0; }}",
        "\treturn -1;",
        "}",
    ]
    return lines


fixture = build_fixture_lines(
    "static int xfer_songdata_write(uint32_t sector, const uint8_t data[8192], void *ctx)",
    "\t__attribute__((noinline, noclone));",
    "emmc_write_blocks(blk, data, 16)",
) + [""] + build_fixture_lines(
    "static int xfer_songdata_write(uint32_t sector, const uint8_t data[8192], void *ctx)",
    None,
    "emmc_write_blocks(blk, data, 16)",
)
# The first block above is a bare forward declaration (never opens a body,
# so it contributes nothing to index_functions()'s output); the second is
# the real definition. This exactly matches main.c's real, current shape:
# forward-declare with a trailing attribute, then define plainly.
func_of_line = index_functions(fixture)
sites = find_call_sites(fixture, func_of_line)
write_sites = {fn: lns for (fn, sym), lns in sites.items() if sym == "emmc_write_blocks"}
check(write_sites == {"xfer_songdata_write": [12]},
      f"index_functions()+find_call_sites(): the real main.c forward-declaration "
      f"shape attributes the emmc_write_blocks() call to the complete, correct "
      f"name (got {write_sites!r})")
check("xfer_songdata_write" in ALLOWED_WRITE_FUNCS,
      "sanity: 'xfer_songdata_write' (the real name) is the exact "
      "ALLOWED_WRITE_FUNCS key")
check("fer_songdata_write" not in ALLOWED_WRITE_FUNCS,
      "sanity: the historically mangled name is NOT an allowed key "
      "(no substring/suffix exception was added to fix this)")

print()

# ============================================================================
# Part 3: negative tests -- lookalike or genuinely unauthorized callers must
# still be rejected by the (unchanged) exact-match against
# ALLOWED_WRITE_FUNCS, proving the parser fix didn't loosen anything.
# ============================================================================

NEGATIVES = [
    ("a truncated name identical to the historical bug's own mangled output",
     "static int fer_songdata_write(uint32_t sector, const uint8_t data[8192], void *ctx)"),
    ("a lookalike name with an extra prefix",
     "static int evil_xfer_songdata_write(uint32_t sector, const uint8_t data[8192], void *ctx)"),
    ("a lookalike name with an extra suffix",
     "static int xfer_songdata_write_unbounded(uint32_t sector, const uint8_t data[8192], void *ctx)"),
    ("a direct call from `main`",
     "int main(void)"),
    ("a direct call from a USB-audio callback",
     "static void uac2_data_recv_cb(const struct device *dev, struct net_buf *buf)"),
    ("a direct call from a MIDI callback",
     "static void midi_rx_packet_cb(const struct device *dev, struct usbd_midi_ump ump)"),
]

for label, sig in NEGATIVES:
    lines = build_fixture_lines(sig, None, "emmc_write_blocks(blk, data, 16)")
    func_of_line = index_functions(lines)
    sites = find_call_sites(lines, func_of_line)
    write_sites = {fn: lns for (fn, sym), lns in sites.items() if sym == "emmc_write_blocks"}
    # The parser must still find SOME real, complete enclosing name here
    # (proving it isn't just failing to classify at all -- see the
    # find_call_sites() precedent of silently skipping fn=None) --
    caller_names = set(write_sites)
    parsed_ok = len(caller_names) == 1
    caller = next(iter(caller_names)) if parsed_ok else None
    allowed = caller in ALLOWED_WRITE_FUNCS if caller is not None else False
    check(parsed_ok and not allowed,
          f"negative: {label} -- parsed enclosing name {caller!r}, "
          f"correctly NOT in ALLOWED_WRITE_FUNCS")

print()

# ============================================================================
# Part 4: end-to-end against the REAL, current firmware/stemtape_player/
# src/main.c -- exactly three emmc_write_blocks() call sites, enclosed by
# exactly the three allowed adapters, each with its real bounds-check
# pattern (lower bound constant, upper bound constant, `return -1;` guard)
# genuinely present in its own body. Run from the repo root, matching every
# other script/test in this repo's own convention.
# ============================================================================

DERIVED_MAIN_C = gate.DERIVED_MAIN_C
if os.path.exists(DERIVED_MAIN_C):
    real_lines = open(DERIVED_MAIN_C, errors="ignore").read().splitlines()
    real_func_of_line = index_functions(real_lines)
    real_sites = find_call_sites(real_lines, real_func_of_line)

    meta_sites = [ln for (fn, sym), lns in real_sites.items()
                  if sym == "meta_write_blocks" for ln in lns]
    check(meta_sites == [], "real main.c: meta_write_blocks() has zero call sites")

    # Stem Tape v1.1 migration, transitional commit: the old v1.0 Gate 2
    # write adapters (xfer_staging_write/xfer_header_write/
    # xfer_songdata_write, ALLOWED_WRITE_FUNCS's three keys) are DELETED
    # from real main.c, not merely disabled -- see this commit's own log.
    # The new v1.1 guarded write path is wired in a FOLLOWING commit, which
    # updates this section again to assert exactly one real
    # emmc_write_blocks() call site inside its own new adapter. Until then,
    # real main.c has ZERO emmc_write_blocks() call sites at all -- no
    # write path, from either contract, is reachable in this image.
    real_write_sites = {fn: lns for (fn, sym), lns in real_sites.items()
                         if sym == "emmc_write_blocks"}
    check(set(real_write_sites) == set(),
          f"real main.c: zero emmc_write_blocks() call sites (v1.0 adapters deleted, "
          f"v1.1 guarded write not yet wired) -- found {sorted(real_write_sites)}")

    # index_functions()'s own values() (not a raw text search, which would
    # also match this very doc comment) are the actual top-level functions
    # DEFINED in real main.c -- confirms the three old v1.0 adapters no
    # longer exist as functions at all, not merely that nothing calls them.
    real_defined_funcs = {fn for fn in real_func_of_line.values() if fn is not None}
    stale_names = sorted(set(ALLOWED_WRITE_FUNCS) & real_defined_funcs)
    check(stale_names == [],
          "real main.c: none of the old v1.0 adapter functions "
          f"({sorted(ALLOWED_WRITE_FUNCS)}) are defined any more -- found {stale_names}")
else:
    print(f"[SKIP] real main.c not found at {DERIVED_MAIN_C} -- run from the repo root "
          f"to exercise part 4 (parts 1-3 already ran against synthetic fixtures)")

print()
failed = [label for ok, label in results if not ok]
print(f"{len(results)} checks, {len(failed)} failed")
if failed:
    print("PARSER SELF-TEST FAILED")
    sys.exit(1)
print("PARSER SELF-TEST PASSED")
sys.exit(0)
