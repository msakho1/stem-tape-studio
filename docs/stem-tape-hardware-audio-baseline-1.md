# Stem Tape hardware audio baseline 1

**The first Stem Tape build whose audio was confirmed by listening on real
hardware.** Every earlier build was CI-green only. That distinction is the
whole point of this document: this is the commit to return to when
something later sounds wrong, and the reference to diff against when
deciding what broke it.

Machine-readable form: `handoff/baselines/stemtape-audio-baseline-1.json`.

## The frozen artifact

| | |
|---|---|
| Tag | `stemtape-audio-baseline-1` |
| Commit | `1143cc9b788f53b4c4f6a37adcfa32c7554cdb15` |
| Branch | `claude/stemtape-m0-safety-audit-1vg9pq` |
| Build tag | `st10` |
| BIN | `stemtape_player.bin` |
| Size | 97,716 bytes |
| **SHA-256** | `a717a21924dc54fd82a69d4cfd6374acceed801417cbb29d3c5536d1501da546` |
| FLASH | 97,716 B of 892 KB (10.70%) |
| RAM | 161,630 B of 256 KB (61.66%), **100,514 B free** |
| Toolchain | Zephyr v4.3.1, SDK 0.17.4, `arm-zephyr-eabi`, board `stem_player` |

The BIN is not in git — it is a CI artifact. Rebuild it from the tag:

```
git checkout stemtape-audio-baseline-1
west build --cmake-only -b stem_player firmware/stemtape_player
cmake --build build-stemtape-player
sha256sum build-stemtape-player/zephyr/stemtape_player.bin
```

### The hash is an anchor, not an accident

Two independent CI runs of this commit, on different runners, produced
**byte-identical** output (runs `32438676107` and `32438678221`). So a
rebuild that does not reproduce `a717a219…da546` means the toolchain or
environment has moved, not that the build is nondeterministic — and that is
worth knowing before blaming the firmware for a behaviour change.

Confirm the device is running this build before trusting any capture: the
boot banner prints `STEMTAPE BUILD st10` and every diagnostic line carries
`b=st10`. If it does not, the flash did not take and the capture says
nothing about this firmware.

## What the companion side of this baseline actually is

**The companion that uploaded the song is a separate application and its
source is not in this repository.** This matters enough to state plainly,
because it is easy to assume otherwise: this repo has a `src/` directory,
and it is *not* the uploader — it contains no SP1XFER or bulk-upload
implementation at all.

What is frozen here is the half that lives in this repo, and it is the half
that determines wire compatibility — the contract the companion was
implemented against, and the fixtures it was verified against:

| artifact | SHA-256 (at the tagged commit) |
|---|---|
| `docs/stem-tape-transfer-v1.1.md` | `257da2e907e1e6d7e28183ed529e5246638bf27be641b551dc8c9305142d2eca` |
| `docs/stem-tape-bulk-upload-v1.md` | `f75da0961f3578b262e7e36f712a93cced12d6872c55fe13700892765dc72dfa` |
| `docs/stem-tape-bulk-upload-handoff.md` | `6c345e3ea75ac247901f8e4fd496c3b574dc5c807e0e7c366b5b35c12b6de28f` |
| `handoff/v1.1/` fixture bundle | git tree `f6e98ab24cea55ee7b04c1d8210d91dfc0c8d11e`, itemised in `handoff/v1.1/SHA256SUMS.txt` |

Those bytes are pinned by the tag and cannot drift.

### The gap, stated honestly

The companion's **own** build identity — its commit, or the deployment URL
and date of the build actually used — is not knowable from inside this
repository, so it is recorded as `null` rather than guessed. The manifest
has fields waiting for it:

```json
"build_identity": { "deployment_url": null, "commit": null, "uploaded_at": null }
```

Until those are filled in, this baseline pins the firmware exactly and the
companion only by contract. That is a real limitation: if a future upload
produces a song this firmware plays badly, the contract hashes will show
whether the *specification* moved, but not whether the companion's
implementation of it did.

The same applies to the song itself. It was 48 kHz, four stems, stereo,
24-bit, in 8192-byte sectors — but its title, duration and byte identity
were not captured at test time, so this baseline is reproducible at the
firmware level and not yet end to end.

Both gaps close by recording the values, not by rebuilding anything.

## What was and was not measured

**Confirmed:** the stored four-stem song played back correctly on real
hardware, by listening.

**Not measured on hardware for this build:**

- worst audio-block execution time (`aus=`)
- achieved sustained read margin (`margin=`)
- whether the summed four-stem mix clips on the song used

The firmware reports all three on its `STEMRT` diagnostic line. Capturing
that line during a future run would upgrade this baseline from *"it sounds
right"* to *"it sounds right, and here is the headroom it had"* — which is
what tells you how close to the edge the next change is allowed to push it.

## Geometry this baseline was proven at

| | |
|---|---|
| Sample rate | 48,000 Hz |
| Bytes per frame | 24 (4 stems × stereo × 3 bytes) |
| Required stream rate | 1,152,000 B/s |
| Sector | 8,192 B = 340 frames = 7.083 ms |
| Audio block | 256 frames = 5.333 ms budget |
| Read-ahead | 12 slots = 11 sectors = 77.9 ms, 98,304 B ring |

## Using this baseline

- **Something sounds wrong later** → flash this BIN. If it sounds right,
  the regression is in the diff since this tag; if it also sounds wrong,
  look at the device, the upload, or the song.
- **Deciding whether a change is safe** → diff against
  `firmware/stemtape_player/src` tree `da4cce94751c1f88a6caaec30f9bd208fc0c6c5a`.
- **Suspecting a companion/wire problem** → compare the four contract
  hashes above. If they match, the specification has not moved.

Note that this baseline is deliberately *not* wired into CI as a
fail-closed hash gate, unlike the golden Tape Looper binary. The Tape
Looper is finished and must never change; Stem Tape is still under active
development, so asserting its BIN hash would fail on the next legitimate
commit. A non-blocking drift report would be the right shape if one is
wanted later.
