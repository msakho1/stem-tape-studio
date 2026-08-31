# Library view and capacity — the companion change, as a handoff to Lovable

Four things were asked for: see what songs are on the SP-1, see how much room
is left, reorder a playlist, and delete songs.

**Two of the four are buildable today with no firmware change at all, and
every byte they need is already on the wire.** The other two are not, because
this firmware holds one song — see "What was NOT asked for here" at the bottom
before building anything beyond this document.

A true multi-song library **is** the intended architecture and is specified in
`docs/stem-tape-v1.3-multi-song-library.md`. It is deliberately not being
started until v1.2 planar playback and per-track reverse are proven on
hardware. This document is the interim, honest view of a one-song device — not
a statement that one song is the destination.

---

## PROMPT FOR LOVABLE — copy everything below this line

Please add a **library view** and a **capacity display** to the companion.
Both are read-only, both use verbs and fields that already exist, and neither
needs a firmware change.

### What this firmware can currently hold

**One song.** Uploading a new one replaces it. A multi-song library is a
planned future firmware, so please build for one song now and do not stub in
a list, a reorder handle, or a delete button that cannot work yet.

Internally the device keeps a second copy of the previous song so an
interrupted upload can roll back — that is why the usable size for one song is
smaller than the raw device capacity. **That mechanism is not something the
normal user should have to think about.** Its only user-visible consequence is
the capacity figure, covered below.

### Where the data comes from

**From the `'Q'` reply** you already parse (the 100-byte `STCP` payload; these
offsets are into the 96-byte body, i.e. after the 4-byte `STCP` tag). All
little-endian u32:

| offset | field |
|---:|---|
| 32 | `deviceBlocks` — total blocks on the device |
| 36 | `songAStart` |
| 40 | `songABlocks` |
| 44 | `songBStart` |
| 48 | `songBBlocks` |
| 52 | `indexAStart` |
| 60 | `indexBStart` |
| 68 | `activeIndexSlot` — 0 = A, 1 = B, `0xffffffff` = none |
| 72 | `activeSongSlot` — 0 = A, 1 = B, `0xffffffff` = none |
| 76 | `activeGenerationLo` |
| 80 | `activeGenerationHi` |

**From two `'R'` block reads.** Read the single block at `indexAStart` and the
single block at `indexBStart`. Each is a STIX v2 index record, which you
already parse — `stemIndex.ts` / `activeIndex.ts`, including the
`expectFormatMinor` you just added. Nothing new to implement: this is the same
record type, read from the same two blocks the transport already touches.

A block whose record fails validation, or whose `SONG_PRESENT` flag is clear,
means that slot holds no usable song. That is a normal state (a fresh device
has both), not an error to surface as a failure.

### What the normal user should see

**The song on the device** — read the record named by `activeSongSlot`:

- **title** and **artist**
- **duration** — `frames / sampleRate`, formatted mm:ss
- **size** — `songBlockCount * 512`

If `activeSongSlot` is `0xffffffff`, or that slot's record fails validation or
has `SONG_PRESENT` clear, the device simply has no song on it yet. Show that
as an ordinary empty state, not an error — a fresh device is legitimately in
it.

**Storage**, as three plain figures:

```
capacity  = songABlocks * 512          // what one song may occupy
used      = songBlockCount * 512       // the current song, 0 if none
free      = capacity - used
```

`songABlocks` and `songBBlocks` are equal, so either can be read as "the space
a song may use". Please present `capacity` as the SP-1's song capacity — do
not add the two regions together and present the sum, because a single song
can never span both, and the sum would promise roughly twice what actually
fits.

### The A/B state: internal, not user-facing

The wire fields for both slots are there and are genuinely useful for
diagnostics — a stuck generation or an unexpected active slot is exactly the
kind of thing worth being able to see when an upload misbehaves. Please put
them behind a **diagnostics / advanced** view rather than in the normal flow:
slot A and B, each with its record's title, generation and validity, and which
one is currently active.

The normal user should never see the words "slot A", "slot B", "generation" or
"rollback".

### Please do NOT

- add the two song regions together when reporting capacity — a song cannot
  span them, and the sum overstates what fits by about 2x;
- build reorder, delete, or "add another song" controls, even disabled or
  "coming soon" — the firmware cannot honour them yet;
- present the rollback copy as a second song in the user's library. It is not
  independently playable and is not something they chose to keep.

### What to send back

The changed files, plus a screenshot of the view in three states: no song on
the device, a song present, and the diagnostics view showing both slots.

## END OF PROMPT

---

## What was NOT asked of Lovable here, and why

**Reorder the playlist.** There is no playlist and no ordering concept
anywhere in the shipping format. The index record describes exactly one song;
the region layout has exactly two song regions. Ordering would be a v1.3
storage design: a new index structure, new capability fields, a new companion
contract, and a migration.

There *is* a multi-slot design in the tree — `src/st_storage_layout.h`'s
`st_library_header_t` with `ST_MAX_SLOTS`, `slot_count`, `current_slot` and a
per-slot `title`/`artist`. It is **deliberately unlinked** (the link-closure
gate lists `st_storage_layout.c` and `st_library_io.c` among the
present-but-unlinked sources). It is v1.0-era and never shipped; the whole
v1.1/v1.2 contract replaced it with A/B. It cannot be "switched on".

**Delete a song.** Refused on purpose today, not by omission:
`st_ab_session.c`'s `candidate_matches_session()` rejects any REPLACE commit
record that does not set `SONG_PRESENT`. Supporting delete means changing that
rule *and* deciding what the boot selector should do with a library that has
been emptied — today "no valid record" means "requires initialization", which
is a different state from "initialized, deliberately empty". That is a small
firmware change but it lands on the A/B rollback invariant, which is the most
heavily tested behaviour in this repository (146 injection points, five
recorded wire transcripts, 1,186 byte-exact assertions). Worth doing
deliberately, not as a side quest.

**Sequencing.** v1.2 has not yet been heard on hardware and per-track reverse
is half-built. Starting a storage-format change now would put two unverified
format migrations in flight simultaneously.
