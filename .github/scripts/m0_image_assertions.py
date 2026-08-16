#!/usr/bin/env python3
"""Stem Tape M0 image assertions (corrected, fail-closed).

Read-only static checks on the built ELF/BIN. Never touches hardware.

SP-1 flash map (firmware/boards/teenageengineering/stem_player/stem_player.dts):
  boot_partition  0x00000000 .. 0x00020000  (TE bootloader)
  slot0_partition 0x00020000 .. 0x000FF000  (application, 0xDF000 bytes)
  storage         0x000FF000 .. 0x00100000

nRF52840 RAM:  0x20000000 .. 0x20040000 (256 KiB)
nRF52840 UICR: 0x10001000 .. 0x10001400

Usage: m0_image_assertions.py <elf> <bin> [--objcopy PATH] [--readelf PATH]
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile

APP_ORIGIN = 0x00020000
APP_SIZE = 0x000DF000
APP_LIMIT = APP_ORIGIN + APP_SIZE          # 0x000FF000
BOOT_LO, BOOT_HI = 0x00000000, 0x00020000
STORAGE_LO, STORAGE_HI = 0x000FF000, 0x00100000
UICR_LO, UICR_HI = 0x10001000, 0x10001400
RAM_START = 0x20000000
RAM_END = 0x20040000
VECTOR_WORDS = 64

failures: list[str] = []


def check(ok: bool, label: str, detail: str) -> None:
    print(f"[{'PASS' if ok else 'FAIL'}] {label}: {detail}")
    if not ok:
        failures.append(f"{label}: {detail}")


def overlaps(lo: int, hi: int, a: int, b: int) -> bool:
    return lo < b and a < hi


def parse_readelf(text: str):
    """Return (sections, segments, entry).

    sections: list of (name, addr, size, flags) for ALLOC sections.
    Flags come from the real 'Flg' column, which in `readelf -W -S` output is
    the 8th whitespace field after the ']' (Name Type Address Off Size ES Flg).
    segments: list of (vaddr, paddr, filesz, memsz) for LOAD segments.
    """
    sections = []
    segments = []
    entry = None
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("Entry point address:"):
            entry = int(line.split(":", 1)[1].strip(), 16)
            continue
        if line.startswith("["):
            body = line.split("]", 1)[1].split()
            # Name Type Addr Off Size ES Flg Lk Inf Al
            if len(body) < 7:
                continue
            name, _typ, addr, _off, size, _es = body[:6]
            flags = body[6]
            if not all(c in "0123456789abcdefABCDEF" for c in addr):
                continue
            try:
                addr_i, size_i = int(addr, 16), int(size, 16)
            except ValueError:
                continue
            # Flg column must not be numeric-only (that would be a misparse).
            if "A" in flags and size_i > 0:
                sections.append((name, addr_i, size_i, flags))
            continue
        if line.startswith("LOAD"):
            f = line.split()
            try:
                segments.append((int(f[2], 16), int(f[3], 16),
                                 int(f[4], 16), int(f[5], 16)))
            except (IndexError, ValueError):
                continue
    return sections, segments, entry


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("binary")
    ap.add_argument("--readelf", default="arm-zephyr-eabi-readelf")
    ap.add_argument("--objcopy", default="arm-zephyr-eabi-objcopy")
    args = ap.parse_args()

    readelf = subprocess.run([args.readelf, "-W", "-h", "-S", "-l", args.elf],
                             check=True, capture_output=True, text=True).stdout
    print(readelf)

    sections, segments, entry = parse_readelf(readelf)
    flash_loads = [(p, p + fsz) for (_v, p, fsz, _m) in segments
                   if fsz and p < RAM_START]

    # 0. parser sanity — a misparse must never look like "no violations"
    check(len(sections) > 0, "0 allocated sections parsed",
          f"count = {len(sections)}")
    check(len(flash_loads) > 0, "0b flash LOAD segments parsed",
          f"count = {len(flash_loads)}")

    # 1. application origin
    origins = [lo for lo, _hi in flash_loads]
    check(bool(origins) and min(origins) == APP_ORIGIN,
          "1 application origin is exactly 0x20000",
          f"lowest flash LMA = {hex(min(origins)) if origins else 'none'}")

    # flash-resident spans: sections by VMA (non-RAM) plus LOAD LMA spans
    flash_spans = [(n, a, a + s) for n, a, s, _f in sections
                   if not (RAM_START <= a < RAM_END)]
    flash_spans += [(f"LOAD@{hex(lo)}", lo, hi) for lo, hi in flash_loads]

    # 2. nothing in the bootloader region
    boot_hits = [(n, hex(a), hex(b)) for n, a, b in flash_spans
                 if overlaps(a, b, BOOT_LO, BOOT_HI)]
    check(not boot_hits, "2 no allocated section or LOAD overlaps the bootloader",
          f"range {hex(BOOT_LO)}..{hex(BOOT_HI)}, violations = {boot_hits or 'none'}")

    # 3. nothing beyond the application limit
    over = [(n, hex(a), hex(b)) for n, a, b in flash_spans if b > APP_LIMIT
            and a < UICR_LO]
    check(not over, "3 no allocated section or LOAD extends beyond the application limit",
          f"limit = {hex(APP_LIMIT)}, violations = {over or 'none'}")

    # 3b. storage partition untouched
    stor = [(n, hex(a), hex(b)) for n, a, b in flash_spans
            if overlaps(a, b, STORAGE_LO, STORAGE_HI)]
    check(not stor, "3b no allocated section or LOAD overlaps the storage partition",
          f"range {hex(STORAGE_LO)}..{hex(STORAGE_HI)}, violations = {stor or 'none'}")

    # 4. no UICR
    uicr = [(n, hex(a), hex(b)) for n, a, b in flash_spans
            if overlaps(a, b, UICR_LO, UICR_HI)]
    uicr += [(n, hex(a), hex(b)) for n, a, b in flash_spans if "uicr" in n.lower()]
    check(not uicr, "4 no UICR section or LOAD present",
          f"range {hex(UICR_LO)}..{hex(UICR_HI)}, matches = {uicr or 'none'}")

    image_size = os.path.getsize(args.binary)
    with open(args.binary, "rb") as fh:
        head = fh.read(4 * VECTOR_WORDS)
    words = struct.unpack("<%dI" % (len(head) // 4), head)

    # 4b. entry point inside the application image
    check(entry is not None and APP_ORIGIN <= (entry & ~1) < APP_ORIGIN + image_size,
          "4b entry point resolves inside the application image",
          f"entry = {hex(entry) if entry is not None else 'unparsed'}, "
          f"image {hex(APP_ORIGIN)}..{hex(APP_ORIGIN + image_size)}")

    # 5. initial stack pointer: inside RAM and 8-byte aligned (AAPCS)
    sp = words[0]
    check(RAM_START < sp <= RAM_END and sp % 8 == 0,
          "5 vector-table stack pointer inside RAM and 8-byte aligned",
          f"SP = {hex(sp)} (RAM {hex(RAM_START)}..{hex(RAM_END)}), sp%8 = {sp % 8}")

    # 6. every populated handler: Thumb bit set and inside the image
    bad = []
    for i, w in enumerate(words[1:], start=1):
        if w == 0 or w == 0xFFFFFFFF:
            continue                      # reserved / unpopulated slot
        if not (w & 1):
            bad.append((i, hex(w), "thumb-bit-clear"))
            continue
        addr = w & ~1
        if not (APP_ORIGIN <= addr < APP_ORIGIN + image_size):
            bad.append((i, hex(w), "out-of-image"))
    check(not bad, "6 every populated vector handler is Thumb and inside the image",
          f"violations = {bad or 'none'}")

    # 7. size within bootloader allowance
    check(image_size <= APP_SIZE, "7 binary size within SP-1 bootloader allowance",
          f"{image_size} bytes / allowance {APP_SIZE} bytes "
          f"({image_size * 100.0 / APP_SIZE:.2f}%)")

    # 8. BIN is byte-for-byte the ELF's own flash contents.
    # Zephyr produces zephyr.bin with an explicit 0xFF gap fill and the same
    # section removals; reproduce those exact conversion rules.
    with tempfile.TemporaryDirectory() as td:
        rebuilt = os.path.join(td, "rebuilt.bin")
        cmd = [args.objcopy,
               "--gap-fill", "0xff",
               "--output-target=binary",
               "--remove-section=.comment",
               "--remove-section=COMMON",
               "--remove-section=.eh_frame",
               args.elf, rebuilt]
        cmd_str = " ".join(cmd)
        rc = subprocess.run(cmd, capture_output=True, text=True)
        if rc.returncode != 0:
            check(False, "8 BIN reconstructed from the ELF is byte-identical",
                  f"objcopy failed: {rc.stderr.strip()[:200]} | command: {cmd_str}")
        else:
            a = open(rebuilt, "rb").read()
            b = open(args.binary, "rb").read()
            if a == b:
                check(True, "8 BIN reconstructed from the ELF is byte-identical",
                      f"{len(a)} bytes identical | command: {cmd_str}")
            else:
                diff = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y),
                            min(len(a), len(b)))
                lo = max(0, diff - 8)
                wa = a[lo:lo + 16].hex(" ")
                wb = b[lo:lo + 16].hex(" ")
                check(False, "8 BIN reconstructed from the ELF is byte-identical",
                      f"sizes {len(a)} vs {len(b)}, first difference at offset "
                      f"{hex(diff)} (ELF address {hex(APP_ORIGIN + diff)})\n"
                      f"    window offset {hex(lo)} (+16 bytes)\n"
                      f"    reconstructed: {wa}\n"
                      f"    zephyr.bin   : {wb}\n"
                      f"    command: {cmd_str}")


    print()
    if failures:
        print("ASSERTIONS FAILED:")
        for f in failures:
            print(" - " + f)
        return 1
    print("ALL IMAGE ASSERTIONS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
