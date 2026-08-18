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
     "static int xfer_v11_write(uint32_t block, const uint8_t data[512])",
     "xfer_v11_write"),
    ("attribute BEFORE the return type, same line",
     "static __attribute__((noinline, noclone)) int xfer_v11_write(uint32_t block, const uint8_t data[512])",
     "xfer_v11_write"),
    ("attribute BETWEEN the return type and the name, same line",
     "static int __attribute__((noinline, noclone)) xfer_v11_write(uint32_t block, const uint8_t data[512])",
     "xfer_v11_write"),
    ("attribute AFTER the parameter list, same line",
     "static int xfer_v11_write(uint32_t block, const uint8_t data[512]) __attribute__((noinline, noclone))",
     "xfer_v11_write"),
    ("attribute BETWEEN return type and name, split onto its OWN line "
     "(the exact shape a real CI run caught misparsing)",
     "static int __attribute__((noinline, noclone))\nxfer_v11_write(uint32_t block, const uint8_t data[512])",
     "xfer_v11_write"),
    ("attribute AFTER the parameter list, on its own continuation line "
     "(this codebase's actual current main.c shape)",
     "static int xfer_v11_write(uint32_t block, const uint8_t data[512])\n\t__attribute__((noinline, noclone))",
     "xfer_v11_write"),
    ("multi-line parameter list, no attribute",
     "static int xfer_v11_write(uint32_t block,\n\tconst uint8_t data[512])",
     "xfer_v11_write"),
    ("multi-line parameter list WITH a trailing attribute on its own line",
     "static int xfer_v11_write(uint32_t block,\n\tconst uint8_t data[512])\n\t__attribute__((noinline, noclone))",
     "xfer_v11_write"),
    ("pointer return type",
     "static uint8_t *xfer_v11_write(uint32_t block, void *ctx)",
     "xfer_v11_write"),
    ("pointer-to-const return type",
     "static const uint8_t *xfer_v11_write(uint32_t block, void *ctx)",
     "xfer_v11_write"),
    ("void-argument, no-parameter function",
     "static void xfer_v11_refresh_session(void)",
     "xfer_v11_refresh_session"),
    ("void-argument function, attribute between return type and name",
     "static void __attribute__((noinline, noclone)) xfer_v11_refresh_session(void)",
     "xfer_v11_refresh_session"),
    ("void-argument function, attribute trailing on its own line",
     "static void xfer_v11_refresh_session(void)\n\t__attribute__((noinline, noclone))",
     "xfer_v11_refresh_session"),
    ("a project macro standing in for a return-type-adjacent attribute "
     "(an object-like macro token, no parens of its own in source -- "
     "exactly how e.g. `#define ST_AUDIT_NOINLINE __attribute__((...))` "
     "would appear UNEXPANDED in the raw source this parser reads)",
     "static ST_AUDIT_NOINLINE int xfer_v11_write(uint32_t block, const uint8_t data[512])",
     "xfer_v11_write"),
    ("xfer_v11_send_caps, a second real v1.1 function using this exact "
     "attribute pattern",
     "static void xfer_v11_send_caps(void)",
     "xfer_v11_send_caps"),
    ("xfer_v11_block_read, a real v1.1 function with a genuinely different "
     "signature shape (three parameters, address-taken, no attribute)",
     "static int xfer_v11_block_read(uint32_t block, uint8_t out[512], void *ctx)",
     "xfer_v11_block_read"),
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
    "static int xfer_v11_write(uint32_t block, const uint8_t data[512])",
    "\t__attribute__((noinline, noclone));",
    "emmc_write_blocks(block, data, 1)",
) + [""] + build_fixture_lines(
    "static int xfer_v11_write(uint32_t block, const uint8_t data[512])",
    None,
    "emmc_write_blocks(block, data, 1)",
)
# The first block above is a bare forward declaration (never opens a body,
# so it contributes nothing to index_functions()'s output); the second is
# the real definition. This exactly matches main.c's real, current shape:
# forward-declare with a trailing attribute, then define plainly.
func_of_line = index_functions(fixture)
sites = find_call_sites(fixture, func_of_line)
write_sites = {fn: lns for (fn, sym), lns in sites.items() if sym == "emmc_write_blocks"}
check(write_sites == {"xfer_v11_write": [12]},
      f"index_functions()+find_call_sites(): the real main.c forward-declaration "
      f"shape attributes the emmc_write_blocks() call to the complete, correct "
      f"name (got {write_sites!r})")
check("xfer_v11_write" in ALLOWED_WRITE_FUNCS,
      "sanity: 'xfer_v11_write' (the real name) is the exact "
      "ALLOWED_WRITE_FUNCS key")
check("fer_v11_write" not in ALLOWED_WRITE_FUNCS,
      "sanity: a historically-mangled-shaped name is NOT an allowed key "
      "(no substring/suffix exception was added to fix this)")

print()

# ============================================================================
# Part 3: negative tests -- lookalike or genuinely unauthorized callers must
# still be rejected by the (unchanged) exact-match against
# ALLOWED_WRITE_FUNCS, proving the parser fix didn't loosen anything.
# ============================================================================

NEGATIVES = [
    ("a truncated name in the shape of the historical bug's own mangled output",
     "static int fer_v11_write(uint32_t block, const uint8_t data[512])"),
    ("a lookalike name with an extra prefix",
     "static int evil_xfer_v11_write(uint32_t block, const uint8_t data[512])"),
    ("a lookalike name with an extra suffix",
     "static int xfer_v11_write_unbounded(uint32_t block, const uint8_t data[512])"),
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
# src/main.c -- exactly ONE emmc_write_blocks() call site, enclosed by the
# one allowed v1.1 write adapter (xfer_v11_write()), with its real safety-
# mechanism pattern (g_v11_layout_ready, st_ab_session_check_write(), an
# early `return -1;` guard) genuinely present in its own body -- and proves
# the retired v1.0 adapters no longer exist as functions at all. Run from
# the repo root, matching every other script/test in this repo's own
# convention.
# ============================================================================

DERIVED_MAIN_C = gate.DERIVED_MAIN_C
if os.path.exists(DERIVED_MAIN_C):
    real_lines = open(DERIVED_MAIN_C, errors="ignore").read().splitlines()
    real_func_of_line = index_functions(real_lines)
    real_sites = find_call_sites(real_lines, real_func_of_line)

    meta_sites = [ln for (fn, sym), lns in real_sites.items()
                  if sym == "meta_write_blocks" for ln in lns]
    check(meta_sites == [], "real main.c: meta_write_blocks() has zero call sites")

    real_write_sites = {fn: lns for (fn, sym), lns in real_sites.items()
                         if sym == "emmc_write_blocks"}
    check(set(real_write_sites) == {"xfer_v11_write"},
          f"real main.c: emmc_write_blocks() call site(s) enclosed by exactly "
          f"the one allowed adapter -- {sorted(real_write_sites)}")
    total_sites = sum(len(v) for v in real_write_sites.values())
    check(total_sites == 1,
          f"real main.c: exactly 1 emmc_write_blocks() call site total (got {total_sites})")

    lower_const, upper_const = ALLOWED_WRITE_FUNCS.get("xfer_v11_write", (None, None))
    lns = real_write_sites.get("xfer_v11_write")
    if lns:
        start, end = function_body_bounds(real_lines, lns[0])
        body = "\n".join(real_lines[start:end])
        has_lower = lower_const in body
        has_upper = upper_const in body
        has_return = re.search(r"return\s+-1\s*;", body) is not None
        check(has_lower and has_upper and has_return,
              f"real main.c: xfer_v11_write()'s own body contains its lower bound "
              f"({lower_const}: {has_lower}), upper bound ({upper_const}: {has_upper}), "
              f"and an early `return -1;` guard ({has_return})")
    else:
        check(False, "real main.c: xfer_v11_write() has a real emmc_write_blocks() call site")

    # index_functions()'s own values() (not a raw text search, which would
    # also match this very doc comment) are the actual top-level functions
    # DEFINED in real main.c -- confirms the retired v1.0 adapters no
    # longer exist as functions at all, not merely that nothing calls them.
    OLD_V10_ADAPTERS = ["xfer_staging_write", "xfer_header_write", "xfer_songdata_write"]
    real_defined_funcs = {fn for fn in real_func_of_line.values() if fn is not None}
    stale_names = sorted(set(OLD_V10_ADAPTERS) & real_defined_funcs)
    check(stale_names == [],
          f"real main.c: none of the old v1.0 adapter functions ({OLD_V10_ADAPTERS}) "
          f"are defined any more -- found {stale_names}")
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
