#!/usr/bin/env python3
"""Stem Tape Player USB descriptor assertion (fail-closed).

The runtime symbol-presence gate (the CI workflow's own bash step) proves
the incoming-MIDI *callback functions* (midi_rx_packet_cb, etc.) are linked
into the final ELF, and separately proves every UAC2 (USB audio class)
symbol has NO definition left to link at all. That is proof at the
callback-glue/symbol-name level; it is deliberately NOT accepted here as
the final word on what the compiled image actually offers a real host.
This script checks two independent, stronger kinds of evidence instead:

  1. The real per-devicetree-instance descriptor-*array*/*struct*/*config*/
     *context* symbols Zephyr's own class macros emit (USBD_MIDI_DEFINE_
     DEVICE in subsys/usb/device_next/class/usbd_midi2.c) -- the literal
     `struct usb_desc_header *[]` arrays and descriptor/config/context
     structs holding the real bInterfaceClass/bInterfaceSubClass/endpoint-
     address fields, not callback glue. Matched by PATTERN (class marker +
     descriptor-shaped term), not one hardcoded exact name: an exact
     per-instance name guessed from reading the macro source once turned
     out wrong for this Zephyr build (confirmed by a real CI run -- the
     instance suffix/naming did not match what a one-off source read
     predicted), so this asks the real, authoritative nm.txt what the
     compiler actually produced instead of re-guessing a literal string a
     second time.

     The SAME pattern-matching machinery is used in the opposite direction
     for UAC2 (USB audio class): STEM TAPE removed UAC2 playback entirely
     (product decision -- it was added earlier under the mistaken
     assumption that KO II cue control depended on it; cue control
     actually uses incoming USB MIDI only). CONFIG_USBD_AUDIO2_CLASS is no
     longer enabled (see prj.conf and the "Effective configuration
     assertion" CI step), so Zephyr's own USBD_UAC2_DT_DEVICE_DEFINE macro
     never expands at all for this build -- this script requires ZERO nm.txt
     symbols matching any UAC2 descriptor/config/context pattern, the
     direct C-level consequence of that removal, not merely the Kconfig
     request.

  2. The expanded, post-CMake-configure devicetree (zephyr.dts) -- proving
     the MIDI input-only Group Terminal Block (usb_midi/cue_in@0) is
     present AND enabled -- status = "okay" explicitly, OR no status
     property at all (which devicetree semantics define as enabled --
     "disabled" is the only value that turns a node off), not merely
     mentioned in the source overlay text. Node bodies are located by real
     brace-depth matching (not a fixed line-count guess), so a short or
     long property list is bounded exactly.

     The UAC2 speaker chain's own devicetree nodes (uac2_speaker/
     in_terminal/speaker_out/as_iso_out) are declared in the SHARED BOARD
     DTS (firmware/boards/teenageengineering/stem_player/stem_player.dts,
     used by every target including the golden Tape Looper, which still
     needs them) -- confirmed directly, before this script was rewritten,
     that they are NOT overlay-local to stemtape_player. Devicetree node
     presence is independent of Kconfig (disabling a USB class does not
     strip its DT node from the compiled devicetree blob), so those nodes
     legitimately still appear in stemtape_player's own zephyr.dts too,
     harmlessly, unclaimed by any driver -- asserting their absence from
     the DTS would be a FALSE claim, and this script does not make it.
     UAC2 removal is proven where it is actually true: no C-level
     descriptor/config/context symbols, no Kconfig class enabled (see
     check 1 above and the Effective configuration assertion step).

A per-Group-Terminal-Block DT `label` (e.g. this target's "SP-1 Cue In")
is deliberately NOT checked as a compiled-in string: a real CI run showed
it is not -- USB-MIDI2 GTB descriptors are numeric (group number, protocol
enum), not string-descriptor-bearing, so the child label is DT/build-time
metadata only, unlike the top-level device `label` (which Zephyr's device
model does compile in, e.g. "Stem Tape Cue In").

Fails closed: any missing required item, or any forbidden UAC2 symbol
present, fails the gate.

Usage: stemtape_player_usb_descriptor_assertions.py <nm.txt> <zephyr.elf> \
           <zephyr.dts> <out-report.md>
"""

from __future__ import annotations

import sys

nm_file, elf_file, dts_file, out_path = sys.argv[1:5]

nm_lines = open(nm_file, errors="ignore").read().splitlines()
elf_bytes = open(elf_file, "rb").read()
dts_lines = open(dts_file, errors="ignore").read().splitlines()

report: list[str] = ["# Stem Tape Player -- USB descriptor assertion", ""]
fail = False

def parse_nm_line(line: str):
    """(type, name) from one `nm -n -S` line, or None. `-S` prints a SIZE
    column for symbols that have one (functions, sized objects) but omits
    it for others (absolute symbols, some externs) -- the field count
    varies per line, so this is NOT a fixed-column parse. Same approach as
    the established stemtape_player_safety_gate.py's parse_nm_line() in
    this repo -- reused deliberately, not reinvented. (A first version of
    this script used a fixed-position regex that silently only matched
    the no-size-column lines -- confirmed by a real CI run's diagnostic
    dump showing solely Kconfig absolute markers matching -- so every real
    function/data symbol with a size column was invisible to it.)"""
    parts = line.split(None, 3)
    if len(parts) == 3:
        _addr, typ, name = parts
        return typ, name
    if len(parts) == 4:
        _addr, _size, typ, name = parts
        return typ, name
    return None


def matching_symbols(required_substrings: tuple[str, ...]) -> list[str]:
    """Every nm symbol whose (lowercased) NAME field contains EVERY string
    in `required_substrings`, excluding type 'A'/'a' (Kconfig build
    markers, e.g. CONFIG_USBD_MIDI2_CLASS -- every one of this script's
    required substring pairs is a coincidental substring of some CONFIG_*
    name, e.g. "midi"+"config" matches CONFIG_..._MIDI2_..._ENABLED
    trivially; real descriptor/config/context objects are never type
    'A')."""
    hits = []
    for line in nm_lines:
        p = parse_nm_line(line)
        if p is None:
            continue
        typ, name = p
        if typ in ("A", "a"):
            continue
        if all(s in name.lower() for s in required_substrings):
            hits.append(f"{typ} {name}")
    return hits


def check_pattern(label: str, required_substrings: tuple[str, ...], min_matches: int = 1):
    """Requires at least `min_matches` matching nm symbols (see
    matching_symbols). Substring matching (not one hardcoded exact name or
    a narrow regex) is deliberate: an exact per-instance name guessed from
    a one-off source read turned out wrong for this Zephyr build (confirmed
    by a real CI run), so this asks nm.txt broadly rather than re-guessing
    a literal string a second time. Prints every match found as evidence."""
    global fail
    hits = matching_symbols(required_substrings)
    if len(hits) >= min_matches:
        report.append(f"present ({label}): {len(hits)} matching symbol(s)")
        report.append("```text")
        report.extend(hits[:20])
        report.append("```")
    else:
        report.append(f"**MISSING** {label}: 0 symbols in nm.txt contained all of "
                      f"{list(required_substrings)}")
        fail = True


def check_pattern_absent(label: str, forbidden_substrings: tuple[str, ...]):
    """The inverse of check_pattern(): requires ZERO matching nm symbols.
    Used for UAC2 (USB audio class) descriptor/config/context material,
    which must have no definition left to link at all now that UAC2
    playback is removed (see this file's own module doc comment)."""
    global fail
    hits = matching_symbols(forbidden_substrings)
    if not hits:
        report.append(f"confirmed absent ({label}): 0 symbols in nm.txt contain all of "
                      f"{list(forbidden_substrings)}")
    else:
        report.append(f"**FORBIDDEN** {label}: {len(hits)} matching symbol(s) still linked "
                      f"(UAC2 must have no descriptor material left in the ELF)")
        report.append("```text")
        report.extend(hits[:20])
        report.append("```")
        fail = True


def check_str(text: str):
    global fail
    if text.encode("utf-8") in elf_bytes or text.encode("utf-16-le") in elf_bytes:
        report.append(f"present in final image: \"{text}\"")
    else:
        report.append(f"**MISSING** expected descriptor/identity string: \"{text}\"")
        fail = True


def node_body_bounds(idx: int) -> tuple[int, int]:
    """Given a 0-based line index inside some devicetree node's body,
    returns [start, end) 0-based bounds of that exact node's brace-
    delimited body -- found by real brace-depth matching (walk outward to
    the nearest enclosing unmatched '{', then forward to its matching
    '}'), not a fixed line-count guess."""
    depth = 0
    start = 0
    for i in range(idx, -1, -1):
        depth += dts_lines[i].count("}") - dts_lines[i].count("{")
        if depth < 0:
            start = i
            break
    depth = 0
    end = len(dts_lines)
    for i in range(start, len(dts_lines)):
        depth += dts_lines[i].count("{") - dts_lines[i].count("}")
        if depth == 0 and i > start:
            end = i + 1
            break
    return start, end


def check_dt_node(label: str, compatible: str, extra: list[str] | None = None):
    """Find `compatible = "<compatible>";` in dts_lines, then require the
    enclosing node body (real brace-bounded, see node_body_bounds) to be
    enabled -- either an explicit `status = "okay";`, or no `status`
    property at all (devicetree default is enabled; only an explicit
    `status = "disabled";` turns a node off) -- and every string in
    `extra` present somewhere in that same body."""
    global fail
    extra = extra or []
    idx = None
    for i, line in enumerate(dts_lines):
        if f'compatible = "{compatible}"' in line:
            idx = i
            break
    if idx is None:
        report.append(f"**MISSING** devicetree node `{label}` "
                      f"(compatible = \"{compatible}\") not found in zephyr.dts")
        fail = True
        return
    start, end = node_body_bounds(idx)
    body = dts_lines[start:end]
    explicit_disabled = any('status = "disabled"' in l for l in body)
    if explicit_disabled:
        report.append(f"**MISSING** `{label}` (compatible = \"{compatible}\", "
                      f"zephyr.dts:{start + 1}-{end}) is explicitly `status = \"disabled\"`")
        fail = True
        return
    missing_extra = [e for e in extra if not any(e in l for l in body)]
    if missing_extra:
        report.append(f"**MISSING** property/properties {missing_extra} on `{label}` "
                      f"(compatible = \"{compatible}\", zephyr.dts:{start + 1}-{end})")
        fail = True
        return
    report.append(f"present + enabled: `{label}` (compatible = \"{compatible}\", "
                  f"zephyr.dts:{start + 1}-{end})" +
                  (f", properties {extra} confirmed" if extra else ""))


report.append("## 1. Real per-instance descriptor symbols "
              "(not callback glue -- the actual descriptor arrays/structs)")
report.append("")
report.append("### UAC2 (USB audio class) -- REMOVED, must be confirmed absent")
report.append("")
check_pattern_absent("UAC2 descriptor array/table", ("uac2", "desc"))
check_pattern_absent("UAC2 per-instance config struct", ("uac2", "cfg"))
check_pattern_absent("UAC2 per-instance context struct", ("uac2", "ctx"))
report.append("")
report.append("### USB-MIDI -- required present")
report.append("")
check_pattern("USB-MIDI descriptor struct/array", ("midi", "desc"))
check_pattern("USB-MIDI per-instance class data", ("midi", "data"))
check_pattern("USB-MIDI per-instance config struct", ("midi", "config"))
report.append("")

report.append("## 2. Descriptor identity strings compiled into the final image")
report.append("")
check_str("Stem Tape Audio")
check_str("softmodded")
check_str("Stem Tape Cue In")
report.append("")

report.append("## 3. Expanded, post-configure devicetree topology (zephyr.dts)")
report.append("")
report.append("(UAC2's own DT nodes -- uac2_speaker/in_terminal/speaker_out/as_iso_out -- "
              "are declared in the shared board DTS, used by every target including the "
              "golden Tape Looper, and legitimately still appear here regardless of this "
              "target's own Kconfig; see this file's own module doc comment. Not checked "
              "here -- their presence proves nothing about this build, and their absence "
              "would be a false claim.)")
report.append("")
check_dt_node("cdc_acm_uart0", "zephyr,cdc-acm-uart")
check_dt_node("usb_midi", "zephyr,midi2-device")
check_dt_node("cue_in@0 (via parent usb_midi)", "zephyr,midi2-device",
              extra=['protocol = "midi1-up-to-128b"', 'terminal-type = "input-only"'])
report.append("")

if fail:
    # Diagnostic-only, not itself a check: every nm.txt symbol mentioning
    # "uac2" or "midi" at all, so a real failure's log carries ground
    # truth for what the compiler actually produced instead of requiring
    # another blind guess-and-CI-round-trip.
    report.append("## Diagnostic: every uac2/midi-related symbol in nm.txt")
    report.append("")
    diag = []
    for line in nm_lines:
        p = parse_nm_line(line)
        if p is None:
            continue
        typ, name = p
        if "uac2" in name.lower() or "midi" in name.lower():
            diag.append(f"{typ} {name}")
    report.append("```text")
    report.extend(diag[:100] if diag else ["(none found)"])
    report.append("```")
    report.append("")

report.append("## Result")
report.append("")
if fail:
    report.append("GATE FAILED -- see missing/forbidden item(s) above.")
else:
    report.append("GATE PASSED -- incoming-only USB-MIDI descriptor material and its "
                  "enabling devicetree topology are confirmed present in the compiled "
                  "image, and UAC2 (USB audio class) descriptor material is confirmed "
                  "absent.")
report.append("")

open(out_path, "w").write("\n".join(report) + "\n")
print("\n".join(report))
sys.exit(1 if fail else 0)
