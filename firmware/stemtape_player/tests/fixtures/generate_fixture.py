#!/usr/bin/env python3
"""Deterministic, license-safe 4-stem test fixture generator.

Generates four synthetic, purely algorithmic mono 16-bit PCM "stems"
matching the REAL current on-flash sample representation implemented by
firmware/stemtape_player/src/main.c (SP1_CODEC_PCM, SAMP_PER_BLK = 256
int16 samples / 512-byte eMMC block -- see main.c's "STORAGE CODEC TOGGLE"
comment). This is NOT yet the stereo/24-bit format described as a follow-up
milestone in main.c's provenance banner; the fixture intentionally matches
what the code actually reads/writes today, not the aspirational target.

Every sample is produced by a closed-form formula from a fixed seed -- no
external audio, no copyrighted material, fully reproducible byte-for-byte
by re-running this script. Re-generate with:

    python3 firmware/stemtape_player/tests/fixtures/generate_fixture.py

FRAME_COUNT_BLOCKS blocks/stem, all four stems the same length (the
st_stem_validate_commit() requirement) -- deliberately small (a few
kibibytes) so the fixture is fast to load in a host test and easy to
inspect by hand, while still exercising a real multi-block CRC32 streaming
computation (matching xfer_service()'s ZBURST-sized read-back bursts in
main.c, not a single-shot digest).

Layout matches the real on-flash unit exactly: each stem is FRAME_COUNT_
BLOCKS * EMMC_BLOCK_SIZE bytes, each block is SAMP_PER_BLK little-endian
int16 samples (memcpy-equivalent PCM, no header, no padding), block-
concatenated with no gaps -- i.e. exactly the bytes emmc_read_blocks()
would hand back for that track's region.

Also emits ONE corrupted variant (stem0_corrupt.bin: a single flipped bit
in the audio data, same length) to exercise CRC-mismatch rejection with a
real captured CRC32 delta, and documents the exact 39-byte 'Z' verb
payload (see main.c xfer_service()) that would commit the valid set as
slot 0.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

EMMC_BLOCK_SIZE = 512
SAMP_PER_BLK = EMMC_BLOCK_SIZE // 2          # 256 int16 samples/block (SP1_CODEC_PCM)
FRAME_COUNT_BLOCKS = 8                        # blocks/stem -> 2048 samples -> ~42.7 ms @ 48 kHz
NTRK = 4
SLOT = 0
BPM_Q8 = int(96.0 * 256)                      # Q8.8, 96.00 BPM
DOWNBEAT_FRAME = 0

FIXTURE_DIR = Path(__file__).resolve().parent


def crc32_ieee(data: bytes) -> int:
    """Same algorithm as st_crc32.c: reflected IEEE 802.3 (poly 0xEDB88320),
    init 0xFFFFFFFF, final XOR 0xFFFFFFFF -- i.e. zlib.crc32."""
    import zlib
    return zlib.crc32(data) & 0xFFFFFFFF


def gen_stem(index: int, n_samples: int) -> bytes:
    """Closed-form, seeded-only waveform per stem -- distinct so a
    swapped-stem bug would change every CRC, not just one."""
    samples = bytearray()
    import math
    freqs = [220.0, 330.0, 110.0, 55.0]        # A2/E4-ish/A1/A0, four clearly distinct tones
    f = freqs[index % len(freqs)]
    amp = 8000  # comfortably inside int16 range, headroom for rounding
    for n in range(n_samples):
        t = n / 48000.0
        if index == 3:
            # stem 3: deterministic PRNG noise (xorshift32, fixed seed) instead
            # of a tone, so the fixture also covers a non-periodic waveform.
            x = (0x2545F491 ^ (n * 2654435761)) & 0xFFFFFFFF
            x ^= (x << 13) & 0xFFFFFFFF
            x ^= (x >> 17)
            x ^= (x << 5) & 0xFFFFFFFF
            v = (x % (2 * amp)) - amp
        else:
            v = int(amp * math.sin(2 * math.pi * f * t))
        samples += struct.pack("<h", v)
    return bytes(samples)


def main():
    n_samples = FRAME_COUNT_BLOCKS * SAMP_PER_BLK
    stems = [gen_stem(i, n_samples) for i in range(NTRK)]
    for s in stems:
        assert len(s) == FRAME_COUNT_BLOCKS * EMMC_BLOCK_SIZE

    manifest = {
        "format": {
            "codec": "SP1_CODEC_PCM (main.c SAMP_PER_BLK default)",
            "sample_format": "mono int16 little-endian, 48000 Hz",
            "emmc_block_size": EMMC_BLOCK_SIZE,
            "samples_per_block": SAMP_PER_BLK,
            "frame_count_blocks": FRAME_COUNT_BLOCKS,
            "duration_ms": round(n_samples / 48000.0 * 1000, 3),
            "ntrk": NTRK,
        },
        "slot": SLOT,
        "bpm_q8": BPM_Q8,
        "downbeat_frame": DOWNBEAT_FRAME,
        "generation": {
            "method": "closed-form per-stem waveform, fixed seed, no external "
                      "audio -- see generate_fixture.py:gen_stem()",
            "stems": [
                "stem0: 220 Hz sine", "stem1: 330 Hz sine",
                "stem2: 110 Hz sine", "stem3: xorshift32 PRNG noise (seed fixed in source)",
            ],
        },
        "stems": [],
    }

    for i, data in enumerate(stems):
        path = FIXTURE_DIR / f"stem{i}.bin"
        path.write_bytes(data)
        manifest["stems"].append({
            "index": i,
            "file": path.name,
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "crc32_ieee_hex": f"0x{crc32_ieee(data):08X}",
        })

    # one corrupted variant of stem0: flip the low bit of the first sample.
    corrupt = bytearray(stems[0])
    corrupt[0] ^= 0x01
    corrupt_path = FIXTURE_DIR / "stem0_corrupt.bin"
    corrupt_path.write_bytes(bytes(corrupt))
    manifest["corrupt_variant"] = {
        "file": corrupt_path.name,
        "of_stem": 0,
        "change": "single bit flip (byte 0, bit 0) in the raw PCM data",
        "bytes": len(corrupt),
        "sha256": hashlib.sha256(bytes(corrupt)).hexdigest(),
        "crc32_ieee_hex": f"0x{crc32_ieee(bytes(corrupt)):08X}",
    }

    # the exact 39-byte 'Z' verb payload (see main.c xfer_service()) that
    # would commit the VALID set as slot 0: slot(1) + frame_count[4](16 LE)
    # + declared_crc32[4](16 LE) + bpm_q8(2 LE) + downbeat_frame(4 LE).
    payload = bytearray()
    payload += struct.pack("<B", SLOT)
    for i in range(NTRK):
        payload += struct.pack("<I", FRAME_COUNT_BLOCKS)
    for i in range(NTRK):
        payload += struct.pack("<I", crc32_ieee(stems[i]))
    payload += struct.pack("<H", BPM_Q8)
    payload += struct.pack("<I", DOWNBEAT_FRAME)
    assert len(payload) == 39, len(payload)
    manifest["z_verb_payload_hex"] = payload.hex()
    manifest["z_verb_payload_bytes"] = len(payload)

    manifest_path = FIXTURE_DIR / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

    print(f"wrote {len(stems)} stem files + 1 corrupt variant + manifest.json to {FIXTURE_DIR}")
    for s in manifest["stems"]:
        print(f"  stem{s['index']}: {s['bytes']} bytes  crc32={s['crc32_ieee_hex']}  sha256={s['sha256'][:16]}...")
    print(f"  corrupt: crc32={manifest['corrupt_variant']['crc32_ieee_hex']} "
          f"(differs from stem0's {manifest['stems'][0]['crc32_ieee_hex']})")


if __name__ == "__main__":
    sys.exit(main())
