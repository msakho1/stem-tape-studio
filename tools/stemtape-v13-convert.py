#!/usr/bin/env python3
"""
stemtape-v13-convert.py -- v1.1/v1.2 24-bit STSC sectors -> v1.3 16-bit.

THIS IS THE EXECUTABLE SPECIFICATION OF THE COMPANION'S CONVERSION.
The companion is TypeScript and lives in another repository, so this cannot
be the code it runs -- but it is the definition it must match, and the file
it produces is what the firmware's own tests decode.  If the two ever
disagree, the null test below is what says so.

THE CONVERSION IS ROUND-TO-NEAREST, AND DELIBERATELY NOT DITHERED.
Measured on the frozen four-stem fixture, rendering both widths through the
production mixdown and differencing:

    truncate      residual -93.3 dBFS   peak 2 LSB   DC bias +0.44
    round         residual -93.5 dBFS   peak 1 LSB   DC bias  none
    TPDF dither   residual -89.3 dBFS   peak 4 LSB

Truncation is genuinely wrong: it biases every sample toward negative
infinity, which is a DC offset on the whole mix.  Dither is wrong for a
different reason -- it decorrelates a FINAL quantisation, and this one is
not final.  The mixer re-quantises to int16 after summing four stems and
that stage is undithered, so it sets the error floor at every level; swept
from 0 dB to -60 dB of source level the dithered residual stayed flat at
-89 dBFS while rounding stayed at -93.  Dither would cost 4 dB and buy
nothing, so the encoder does not carry an RNG.

GEOMETRY. Both widths tile the same containers with zero padding:

    24-bit   32 + 340 * 24 == 8192      8 + 340 * 6 == 2048
    16-bit   32 + 510 * 16 == 8192      8 + 510 * 4 == 2048

so a sector is still a sector and a group is still a group; only the frame
count inside them changes.  That is why this is a payload migration and not
a layout redesign.

USAGE
    python3 tools/stemtape-v13-convert.py IN_24BIT.bin OUT_16BIT.bin
    python3 tools/stemtape-v13-convert.py --null-test IN_24BIT.bin
"""

import argparse
import math
import struct
import sys

SECTOR_BYTES = 8192
SECTOR_HDR = 32
STEMS = 4
CHANS = 2

OFF_MAGIC, OFF_INDEX, OFF_FIRST, OFF_COUNT = 0, 4, 8, 12
OFF_BPM, OFF_DOWNBEAT = 16, 20
STSC = 0x53545343

FR24 = (SECTOR_BYTES - SECTOR_HDR) // (STEMS * CHANS * 3)   # 340
FR16 = (SECTOR_BYTES - SECTOR_HDR) // (STEMS * CHANS * 2)   # 510
assert SECTOR_HDR + FR24 * STEMS * CHANS * 3 == SECTOR_BYTES
assert SECTOR_HDR + FR16 * STEMS * CHANS * 2 == SECTOR_BYTES


def i24(b, o):
    v = b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)
    return v - 0x1000000 if v & 0x800000 else v


def to16(v):
    """Q23 -> int16, round to nearest, saturating.

    (v + 128) >> 8 rounds half away from negative infinity, which is
    unbiased across the signed range -- unlike >> 8 alone, whose truncation
    toward -inf is the +0.44 LSB DC offset measured above.
    """
    q = (v + 128) >> 8
    return 32767 if q > 32767 else (-32768 if q < -32768 else q)


def read_24(path):
    """-> (frames, meta) where frames is a flat list of 8 ints per frame."""
    raw = open(path, "rb").read()
    if len(raw) % SECTOR_BYTES:
        sys.exit(f"{path}: {len(raw)} bytes is not a whole number of 8192-byte sectors")
    frames, meta = [], None
    for s in range(len(raw) // SECTOR_BYTES):
        base = s * SECTOR_BYTES
        magic, idx, first, count, bpm, down = struct.unpack_from("<6I", raw, base)
        if magic != STSC:
            sys.exit(f"sector {s}: magic {magic:#x} is not 'STSC' -- not a v1.1/v1.2 song")
        if meta is None:
            meta = (bpm, down)
        if count > FR24:
            sys.exit(f"sector {s}: frame_count {count} exceeds {FR24}")
        for f in range(count):
            fo = base + SECTOR_HDR + f * STEMS * CHANS * 3
            frames.append([i24(raw, fo + k * 3) for k in range(STEMS * CHANS)])
    return frames, meta


def write_16(path, frames, meta):
    bpm, down = meta
    total = len(frames)
    nsec = (total + FR16 - 1) // FR16
    out = bytearray()
    for s in range(nsec):
        first = s * FR16
        count = min(FR16, total - first)
        hdr = bytearray(SECTOR_HDR)
        struct.pack_into("<6I", hdr, 0, STSC, s, first, count, bpm, down)
        out += hdr
        body = bytearray((SECTOR_BYTES - SECTOR_HDR))
        for f in range(count):
            fo = f * STEMS * CHANS * 2
            for k, v in enumerate(frames[first + f]):
                struct.pack_into("<h", body, fo + k * 2, to16(v))
        out += body
    open(path, "wb").write(out)
    return nsec, total


def null_test(frames):
    """The acceptance gate: render both widths through the production
    mixdown (unity gain, sum, reduce to int16, saturate) and difference."""
    def sat(v):
        return 32767 if v > 32767 else (-32768 if v < -32768 else v)

    sq = pk = n = 0
    ref_sq = 0
    for fr in frames:
        for ch in (0, 1):
            a = sat(sum(fr[s * 2 + ch] for s in range(STEMS)) >> 8)
            b = sat(sum(to16(fr[s * 2 + ch]) for s in range(STEMS)))
            e = a - b
            sq += e * e
            ref_sq += a * a
            pk = max(pk, abs(e))
            n += 1
    res = 20 * math.log10(math.sqrt(sq / n) / 32768.0) if sq else float("-inf")
    ref = 20 * math.log10(math.sqrt(ref_sq / n) / 32768.0)
    print(f"  reference mix   {ref:+7.1f} dBFS RMS")
    print(f"  residual        {res:+7.1f} dBFS RMS   peak error {pk} LSB")
    GATE = -90.0
    ok = res <= GATE
    print(f"  gate            {GATE:+7.1f} dBFS        {'PASS' if ok else 'FAIL'}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst", nargs="?")
    ap.add_argument("--null-test", action="store_true")
    a = ap.parse_args()

    frames, meta = read_24(a.src)
    print(f"{a.src}: {len(frames)} frames, {len(frames)/FR24:.1f} sectors at 24-bit")

    if a.null_test or not a.dst:
        sys.exit(0 if null_test(frames) else 1)

    nsec, total = write_16(a.dst, frames, meta)
    print(f"{a.dst}: {total} frames, {nsec} sectors at 16-bit "
          f"({nsec * SECTOR_BYTES} bytes, {100 - 100*nsec/(len(frames)/FR24):.0f}% smaller)")
    if not null_test(frames):
        sys.exit(1)


if __name__ == "__main__":
    main()
