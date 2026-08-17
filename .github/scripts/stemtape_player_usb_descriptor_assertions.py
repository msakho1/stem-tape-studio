#!/usr/bin/env python3
"""Stem Tape Player GATE 1 -- USB descriptor assertion (fail-closed).

The runtime symbol-presence gate (the CI workflow's own bash step) proves
the UAC2/MIDI *callback functions* (uac2_data_recv_cb, midi_rx_packet_cb,
etc.) are linked into the final ELF. That is proof the glue code exists --
it is deliberately NOT accepted here as proof that the compiled image
actually offers a CDC-console + UAC2-speaker + incoming-USB-MIDI *USB
configuration* to a real host. This script checks three independent,
stronger kinds of evidence instead:

  1. The real per-devicetree-instance descriptor-*array* / descriptor-
     *struct* symbols that Zephyr's own class macros emit -- verified
     against Zephyr v4.3.0 subsys/usb/device_next/class/usbd_uac2.c
     (USBD_UAC2_DT_DEVICE_DEFINE -> `uac2_fs_desc_<n>`, `uac2_cfg_<n>`,
     `uac2_ctx_<n>`) and usbd_midi2.c (USBD_MIDI_DEFINE_DEVICE ->
     `usbd_midi_desc_<n>`, `usbd_midi_desc_array_fs_<n>`,
     `usbd_midi_data_<n>`, `usbd_midi_config_<n>`). These are the literal
     `struct usb_desc_header *[]` arrays and descriptor structs holding the
     real bInterfaceClass/bInterfaceSubClass/endpoint-address fields, not
     callback glue -- a symbol name that can only exist if the descriptor
     material itself was compiled in.

  2. Literal descriptor label/identity strings that only end up in the
     final .rodata if the corresponding descriptor code path and
     devicetree node were actually built: the USB product/manufacturer
     strings (prj.conf) and this target's own USB-MIDI Group Terminal
     Block labels (app.overlay).

  3. The expanded, post-CMake-configure devicetree (zephyr.dts) -- proving
     the UAC2 speaker chain (uac2_speaker/in_terminal/speaker_out/
     as_iso_out) and the MIDI input-only Group Terminal Block
     (usb_midi/cue_in@0) are each present AND enabled (status = "okay"),
     not merely mentioned in the source overlay text.

Fails closed: any missing symbol, string, or DT node/status pairing fails
the gate.

Usage: stemtape_player_usb_descriptor_assertions.py <nm.txt> <zephyr.elf> \
           <zephyr.dts> <out-report.md>
"""

from __future__ import annotations

import re
import sys

nm_file, elf_file, dts_file, out_path = sys.argv[1:5]

nm_lines = open(nm_file, errors="ignore").read().splitlines()
elf_bytes = open(elf_file, "rb").read()
dts_lines = open(dts_file, errors="ignore").read().splitlines()

report: list[str] = ["# Stem Tape Player -- USB descriptor assertion (GATE 1)", ""]
fail = False


def sym_present(name: str) -> bool:
    rx = re.compile(r"^\S+\s+\S\s+" + re.escape(name) + r"$")
    return any(rx.match(line) for line in nm_lines)


def check_sym(name: str):
    global fail
    if sym_present(name):
        report.append(f"present (descriptor symbol): `{name}`")
    else:
        report.append(f"**MISSING** required descriptor symbol: `{name}`")
        fail = True


def check_str(text: str):
    global fail
    if text.encode("utf-8") in elf_bytes or text.encode("utf-16-le") in elf_bytes:
        report.append(f"present in final image: \"{text}\"")
    else:
        report.append(f"**MISSING** expected descriptor/identity string: \"{text}\"")
        fail = True


def check_dt_node(label: str, compatible: str, extra: list[str] | None = None):
    """Find `compatible = "<compatible>";` in dts_lines, then require
    `status = "okay";` and every string in `extra` within the following
    30 lines (same node body, in the expanded per-node DTS block)."""
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
    window = dts_lines[idx: idx + 30]
    ok = any('status = "okay"' in l for l in window)
    if not ok:
        report.append(f"**MISSING** `{label}` (compatible = \"{compatible}\") found "
                      f"at zephyr.dts:{idx + 1} but no `status = \"okay\";` in its body")
        fail = True
        return
    missing_extra = [e for e in extra if not any(e in l for l in window)]
    if missing_extra:
        report.append(f"**MISSING** property/properties {missing_extra} on `{label}` "
                      f"(compatible = \"{compatible}\", zephyr.dts:{idx + 1})")
        fail = True
        return
    report.append(f"present + enabled: `{label}` (compatible = \"{compatible}\", "
                  f"zephyr.dts:{idx + 1}) -- status \"okay\"" +
                  (f", properties {extra} confirmed" if extra else ""))


report.append("## 1. Real per-instance descriptor symbols "
              "(not callback glue -- the actual descriptor arrays/structs)")
report.append("")
check_sym("uac2_fs_desc_0")
check_sym("uac2_cfg_0")
check_sym("uac2_ctx_0")
check_sym("usbd_midi_desc_0")
check_sym("usbd_midi_desc_array_fs_0")
check_sym("usbd_midi_data_0")
check_sym("usbd_midi_config_0")
report.append("")

report.append("## 2. Descriptor identity strings compiled into the final image")
report.append("")
check_str("Stem Tape Audio")
check_str("softmodded")
check_str("Stem Tape Cue In")
check_str("SP-1 Cue In")
report.append("")

report.append("## 3. Expanded, post-configure devicetree topology (zephyr.dts)")
report.append("")
check_dt_node("uac2_speaker", "zephyr,uac2")
check_dt_node("in_terminal", "zephyr,uac2-input-terminal")
check_dt_node("speaker_out", "zephyr,uac2-output-terminal")
check_dt_node("as_iso_out", "zephyr,uac2-audio-streaming")
check_dt_node("cdc_acm_uart0", "zephyr,cdc-acm-uart")
check_dt_node("usb_midi", "zephyr,midi2-device")
check_dt_node("cue_in@0 (via parent usb_midi)", "zephyr,midi2-device",
              extra=['protocol = "midi1-up-to-128b"', 'terminal-type = "input-only"'])
report.append("")

report.append("## Result")
report.append("")
if fail:
    report.append("GATE FAILED -- see missing item(s) above.")
else:
    report.append("GATE PASSED -- real UAC2 speaker (playback) and incoming-only "
                  "USB-MIDI descriptor material, and their enabling devicetree "
                  "topology, are confirmed present in the compiled image.")
report.append("")

open(out_path, "w").write("\n".join(report) + "\n")
print("\n".join(report))
sys.exit(1 if fail else 0)
