# Retiring the v1.1 audio-decode conformance gate

Explicit, so that coverage is not reduced silently. This document is the
record; the code points at it from every place a claim was withdrawn.

## The decision

v1.3 is a **breaking storage-format migration**. It fails closed on v1.1 and
v1.2 media by design — a v1.1 sector read as four planar groups is not
garbage, it is one stem's timeline played as all four at full scale, and a
v1.2 group read at 16 bits is noise at full scale. Neither fails safe by
accident, so both are refused on the declared version before a single audio
byte is fetched.

The firmware therefore **must not carry a 24-bit decoder**. It is a path the
shipped build can never take, and the only place it could live is
`st_pl_decode_stem_inline()` — the innermost loop of the 48 kHz render, and
the one function where a future reader most needs to see exactly one way for a
sample to be read.

A conformance gate that required that decoder to exist was rescoped rather
than served.

## What was retired

### 1. Byte-exact v1.1 audio decoding — `tests/test_stem_v11.c`

**Was:** decode the frozen 24-bit `handoff/v1.1/binaries/song-sectors-four-stem.bin`
and match all four per-stem FNV-1a checksums the **companion** declared —
values computed on the other side of the contract, by a different
implementation, before this firmware existed.

**Now:** the same walk over the same audio at the width this firmware stores,
against `handoff/v1.3/binaries/song-sectors-four-stem.bin` and the checksums in
`handoff/v1.3/decoded/song-sectors-four-stem.json`.

**What is genuinely weaker.** The v1.3 checksums are computed by an
*independent implementation of the algorithm* (see that manifest's own `note`),
not by the other side of the original contract. It is still two
implementations agreeing; it is no longer two parties agreeing.

**What is undiminished.** The container half: every sector's magic, sequential
index, `first_frame`, the full/short `frame_count`, the sum over all sectors
equalling the declared song length, the tempo grid surviving conversion, and
the decode being losslessly re-encodable.

### 2. Commit-time audio verification inside the transcript replay — `tests/test_stem_v11_transcripts.c`

**Was:** `replay_write()` ran the real `st_ab_session_verify_song_before_commit()`
over the bytes the transcript had just written and gated the commit magic on
it, mirroring `main.c`'s `xfer_v11_write()`.

**Now:** the commit proceeds without that gate. The transcripts are byte-exact
recordings of a **v1.1** upload, so the song they write is 24-bit interleaved
audio; a v1.3 build refuses it, which is the fail-closed behaviour this
migration is for.

**Where it is still proven, at production settings:**
`test_ab_session_open_and_negative_writes` (the magic write is REFUSED before
verification and ACCEPTED after) and `test_ab_session_v12_planar_verification`
(the verifier's positive, tampered-checksum, mislabelled-stem, wrong-flags,
gap-invalidation, idempotent-retry and version-dispatch cases).

### 3. Post-replay audio re-verification — same file, `reverify_song()`

**Was:** decode every sector of the selected song region from final storage and
match all five declared checksums.

**Now:** the same five call sites check what they were actually guarding — that
an *interrupted* replacement did not damage storage:

- the record's declared geometry is self-consistent and `SONG_PRESENT`;
- the region holds real content (not erased, not blank);
- the region is **distinct from the other slot's**, which is exactly what
  "untouched by generation 3's own, independent write" means.

A cross-write, a truncation or an erase still fails. **A single flipped sample
no longer does.** That is the honest cost.

## What was explicitly preserved

Everything on this list runs at **production settings** (no build overrides)
unless marked:

| Coverage | Where |
|---|---|
| STIX v2 parsing, CRC rule, field validation | `test_stem_v11.c` |
| A/B selector, generation compare, rollback, corrupt-newest fallback | `test_stem_v11.c` |
| Commit-magic ordering, torn writes, single-use closure | `test_stem_v11.c` |
| `magic-write-cases.json` cross-contract cases | `test_stem_v11.c` |
| `successive-replacement.json` | `test_stem_v11.c` |
| STCP capability reply, byte-for-byte | `test_stem_v11.c`, now against the **v1.3** fixture — the reply this firmware actually sends |
| **Explicit rejection of incompatible versions** | `test_stem_v11.c`, `test_incompatible_versions_are_refused` — new, and only possible now |
| 171 recorded wire frames → real ACK/NAK, every frame | `test_stem_v11_transcripts.c` * |
| 146-point interruption sweep, recorded outcomes | `test_stem_v11_transcripts.c` * |
| Two-upload successive replacement over one device | `test_stem_v11_transcripts.c` * |

\* the transcript harness compiles with `-DST11_FORMAT_MINOR=1u
-DST11_PROTOCOL_MINOR=1u`. Both are **version integers**. Neither duplicates a
code path, and in particular neither reinstates a second way to read a sample.

The new rejection case is worth calling out: while the harness was built at
`-DST11_FORMAT_MINOR=1u` it *could not ask the question at all* — it was a
v1.1 build, so of course it accepted v1.1 records. Running at the shipped
version turns the frozen v1.1 fixtures into what they now are, media from a
format this firmware refuses, and makes the refusal itself the thing under
test. That is coverage the old arrangement could not have.

## The one production change this required

`st_stix_validate()` checked a record's declared `bit_depth` against
`ST11_PCM_BIT_DEPTH`, the build's constant. It now checks it against
`ST11_BIT_DEPTH_FOR_FORMAT(record->format_minor)` — the width the record's own
declared format implies.

For shipped firmware the two are identical: the version check immediately
above already requires `format_minor == ST11_FORMAT_MINOR`, so the width check
can only pass at `ST11_PCM_BIT_DEPTH`. What it buys is that the transcript
harness can validate recorded v1.1 records without the build being handed a
24-bit width — which is exactly what would have dragged the decoder back in.

It is also simply more correct: the stored width is not an independent field a
record may choose, and a record disagreeing with its own version is malformed.

## Residual risk

1. **No cross-party check on the audio codec.** If both this firmware's decoder
   and `tools/stemtape-v13-convert.py` were wrong in the same way, the v1.3
   fixture case would pass. The companion's own encoder is the third
   implementation that closes this, and re-establishing a byte-exact
   companion-derived v1.3 fixture is the cheapest way to buy the lost strength
   back — worth doing when the companion ships its v1.3 encoder.
2. **Single-sample corruption in an interrupted upload** is no longer caught by
   the transcript replay's post-hoc check. Region-level damage still is.
3. **The transcripts can never be re-recorded at v1.3** without a v1.3
   companion producing a new session. Until then they prove the write gate,
   not the payload.

## How to reverse this

Delete `ST11_BIT_DEPTH_FOR_FORMAT` and restore the two retired verifications
only if a 24-bit decode path is reintroduced — and it should not be. The
better path forward is (1) above: a companion-produced v1.3 fixture, which
restores the strongest lost claim without any compatibility code at all.
