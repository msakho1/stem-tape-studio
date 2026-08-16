#!/usr/bin/env python3
"""Self-test fixture for m0_image_assertions.py.

Proves the assertion script is fail-closed: for every assertion there is a
deliberately violated fixture that MUST make the script exit non-zero, plus a
baseline fixture that MUST pass. No toolchain and no hardware are needed: a
stub readelf (prints a canned report) and a stub objcopy (copies a canned
image) stand in for the real binutils.

Exit code 0 = the assertion script behaves correctly.
"""

from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ASSERT = os.path.join(HERE, "m0_image_assertions.py")

APP_ORIGIN = 0x20000
GOOD_SP = 0x20040000
IMAGE_WORDS = 4096            # 16 KiB image


def readelf_report(sections, loads, entry=APP_ORIGIN + 0x101):
    out = [
        "ELF Header:",
        f"  Entry point address:               {hex(entry)}",
        "Section Headers:",
        "  [Nr] Name Type Address Off Size ES Flg Lk Inf Al",
    ]
    for i, (name, addr, size, flg) in enumerate(sections):
        # Name Type Address Off Size ES Flg Lk Inf Al  (ES is 00 on purpose:
        # the old parser read this column as flags and saw no 'A').
        out.append(f"  [{i:2}] {name:16} PROGBITS {addr:08x} 001000 "
                   f"{size:06x} 00 {flg}  0   0  4")
    out.append("Program Headers:")
    out.append("  Type Offset VirtAddr PhysAddr FileSiz MemSiz Flg Align")
    for (v, p, fsz, msz) in loads:
        out.append(f"  LOAD 0x001000 0x{v:08x} 0x{p:08x} 0x{fsz:06x} "
                   f"0x{msz:06x} RE 0x1000")
    return "\n".join(out) + "\n"


def make_image(sp=GOOD_SP, handlers=None, size_words=IMAGE_WORDS):
    words = [sp]
    handlers = handlers or {}
    for i in range(1, size_words):
        words.append(handlers.get(i, (APP_ORIGIN + 0x101) | 0
                                  if i < 64 else 0))
    # default: every vector slot 1..63 points at a valid thumb address
    for i in range(1, 64):
        if i not in handlers:
            words[i] = (APP_ORIGIN + 0x100) | 1
    return struct.pack("<%dI" % size_words, *[w & 0xFFFFFFFF for w in words])


def run_case(name, sections, loads, image, entry=APP_ORIGIN + 0x101,
             objcopy_image=None, expect_pass=False):
    with tempfile.TemporaryDirectory() as td:
        report = os.path.join(td, "readelf.txt")
        open(report, "w").write(readelf_report(sections, loads, entry))
        binpath = os.path.join(td, "image.bin")
        open(binpath, "wb").write(image)
        oc_src = os.path.join(td, "objcopy_src.bin")
        open(oc_src, "wb").write(objcopy_image if objcopy_image is not None else image)

        fake_readelf = os.path.join(td, "readelf")
        open(fake_readelf, "w").write(f'#!/bin/sh\ncat "{report}"\n')
        os.chmod(fake_readelf, 0o755)
        fake_objcopy = os.path.join(td, "objcopy")
        # The real command ends with <elf> <out>; copy the canned image to the
        # last argument so the stub is independent of the flag list.
        open(fake_objcopy, "w").write(
            '#!/bin/sh\nfor a in "$@"; do out="$a"; done\n'
            f'cp "{oc_src}" "$out"\n')
        os.chmod(fake_objcopy, 0o755)


        elf = os.path.join(td, "zephyr.elf")
        open(elf, "wb").write(b"\x7fELF")

        r = subprocess.run(
            [sys.executable, ASSERT, elf, binpath,
             "--readelf", fake_readelf, "--objcopy", fake_objcopy],
            capture_output=True, text=True)
        passed = r.returncode == 0
        ok = passed == expect_pass
        print(f"[{'OK  ' if ok else 'BAD '}] {name}: "
              f"exit={r.returncode} expected={'0' if expect_pass else 'non-zero'}")
        if not ok:
            print(r.stdout[-3000:])
            print(r.stderr[-2000:])
        return ok


IMG = make_image()
IMG_SIZE = len(IMG)

GOOD_SECTIONS = [
    ("text", APP_ORIGIN, IMG_SIZE, "AX"),
    ("bss", 0x20000000, 0x800, "WA"),
]
GOOD_LOADS = [
    (APP_ORIGIN, APP_ORIGIN, IMG_SIZE, IMG_SIZE),
    (0x20000000, APP_ORIGIN + IMG_SIZE, 0x100, 0x800),
]

cases = []
cases.append(("baseline (all assertions must pass)",
              GOOD_SECTIONS, GOOD_LOADS, IMG, APP_ORIGIN + 0x101, None, True))

# 0: no allocated sections parsed
cases.append(("0 zero allocated sections",
              [("text", APP_ORIGIN, IMG_SIZE, "X")], GOOD_LOADS, IMG,
              APP_ORIGIN + 0x101, None, False))
# 1: wrong origin
cases.append(("1 origin not 0x20000",
              [("text", 0x21000, IMG_SIZE, "AX")],
              [(0x21000, 0x21000, IMG_SIZE, IMG_SIZE)], IMG,
              0x21101, None, False))
# 2: bootloader overlap
cases.append(("2 section inside bootloader region",
              GOOD_SECTIONS + [("boot", 0x1000, 0x100, "A")], GOOD_LOADS, IMG,
              APP_ORIGIN + 0x101, None, False))
# 3: past application limit
cases.append(("3 section beyond application limit",
              GOOD_SECTIONS + [("big", 0xFE000, 0x4000, "A")], GOOD_LOADS, IMG,
              APP_ORIGIN + 0x101, None, False))
# 3b: storage overlap
cases.append(("3b section inside storage partition",
              GOOD_SECTIONS + [("stor", 0xFF000, 0x10, "A")], GOOD_LOADS, IMG,
              APP_ORIGIN + 0x101, None, False))
# 4: UICR present
cases.append(("4 UICR section present",
              GOOD_SECTIONS + [("uicr", 0x10001000, 0x10, "A")], GOOD_LOADS, IMG,
              APP_ORIGIN + 0x101, None, False))
# 4b: entry outside image
cases.append(("4b entry point outside the image",
              GOOD_SECTIONS, GOOD_LOADS, IMG, 0x00001001, None, False))
# 5a: SP outside RAM
cases.append(("5 stack pointer outside RAM",
              GOOD_SECTIONS, GOOD_LOADS, make_image(sp=0x00020000),
              APP_ORIGIN + 0x101, None, False))
# 5b: SP misaligned
cases.append(("5 stack pointer not 8-byte aligned",
              GOOD_SECTIONS, GOOD_LOADS, make_image(sp=0x2003FFFC),
              APP_ORIGIN + 0x101, None, False))
# 6a: thumb bit clear
cases.append(("6 vector handler with Thumb bit clear",
              GOOD_SECTIONS, GOOD_LOADS,
              make_image(handlers={3: APP_ORIGIN + 0x200}),
              APP_ORIGIN + 0x101, None, False))
# 6b: handler outside the image
cases.append(("6 vector handler outside the image",
              GOOD_SECTIONS, GOOD_LOADS,
              make_image(handlers={5: 0x00001001}),
              APP_ORIGIN + 0x101, None, False))
# 7: oversized image
BIG = make_image(size_words=(0xDF000 // 4) + 16)
BIG_SECTIONS = [("text", APP_ORIGIN, len(BIG), "AX")]
BIG_LOADS = [(APP_ORIGIN, APP_ORIGIN, len(BIG), len(BIG))]
cases.append(("7 binary larger than the bootloader allowance",
              BIG_SECTIONS, BIG_LOADS, BIG, APP_ORIGIN + 0x101, None, False))
# 8: BIN differs from the ELF
tampered = bytearray(IMG)
tampered[0x400] ^= 0xFF
cases.append(("8 BIN not byte-identical to the ELF",
              GOOD_SECTIONS, GOOD_LOADS, IMG, APP_ORIGIN + 0x101,
              bytes(tampered), False))

# 8b/8c: NOBITS (tbss) / alignment gap must be reconstructed with 0xFF fill.
# The shipped zephyr.bin carries 0xFF across the gap; a default/zero-filled
# reconstruction must be rejected.
GAP_OFF = 0xC424                      # matches the observed tbss gap offset
GAP_LEN = 0x40
_gap_img = bytearray(make_image(size_words=(GAP_OFF + 0x1000) // 4))
_gap_img[GAP_OFF:GAP_OFF + GAP_LEN] = b"\xff" * GAP_LEN
GAP_IMG = bytes(_gap_img)
_zero = bytearray(GAP_IMG)
_zero[GAP_OFF:GAP_OFF + GAP_LEN] = b"\x00" * GAP_LEN
GAP_SECTIONS = [("text", APP_ORIGIN, len(GAP_IMG), "AX"),
                ("tbss", APP_ORIGIN + GAP_OFF, GAP_LEN, "WAT")]
GAP_LOADS = [(APP_ORIGIN, APP_ORIGIN, len(GAP_IMG), len(GAP_IMG))]
cases.append(("8b NOBITS/alignment gap reconstructed with 0xFF gap fill",
              GAP_SECTIONS, GAP_LOADS, GAP_IMG, APP_ORIGIN + 0x101,
              GAP_IMG, True))
cases.append(("8c NOBITS/alignment gap reconstructed with zero gap fill",
              GAP_SECTIONS, GAP_LOADS, GAP_IMG, APP_ORIGIN + 0x101,
              bytes(_zero), False))


results = [run_case(*c) for c in cases]
print()
if all(results):
    print(f"SELF-TEST PASSED ({len(results)} fixtures)")
    sys.exit(0)
print(f"SELF-TEST FAILED ({results.count(False)} of {len(results)} fixtures wrong)")
sys.exit(1)
