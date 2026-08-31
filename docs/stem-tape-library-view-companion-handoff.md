# Library view and capacity — the companion change, as a handoff to Lovable

Four things were asked for: see what songs are on the SP-1, see how much room
is left, reorder a playlist, and delete songs.

**Two of the four are buildable today with no firmware change at all, and
every byte they need is already on the wire.** The other two are not, because
the device does not hold a playlist — see "What was NOT asked for here" at the
bottom before building anything beyond this document.

---

## PROMPT FOR LOVABLE — copy everything below this line

Please add a **library view** and a **capacity display** to the companion.
Both are read-only, both use verbs and fields that already exist, and neither
needs a firmware change.

### First, what the SP-1 actually stores — the UI must not imply otherwise

The device is **not** a playlist player. Its storage is four fixed regions:

```
| index A (1 block) | index B (1 block) |   song A   |   song B   |
```

It holds **one playable song**. The other song region holds the **previous
song**, kept deliberately so an interrupted or failed upload can roll back to
it. Uploading does not "add" a song — it writes into whichever song region is
currently inactive, verifies it, and then atomically switches which one is
active by committing an index record.

So the honest thing to show is **two slots, one of them current**, not a list
that looks like it could grow. Please don't build an "add song" affordance
that implies a third can exist.

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

### What to show

**Per slot (A and B):**

- title, artist (both already in the record)
- duration — `frames / sampleRate`, formatted mm:ss
- `generation` — higher wins; this is what makes one of them current
- size on device — `songBlockCount * 512` bytes
- a clear **CURRENT** marker on the slot matching `activeSongSlot`
- for the other slot, label it **previous — kept for rollback**, not
  "song 2". It is not independently playable; it is the safety copy.

**Capacity.** Please show this per slot, not as one pool:

```
songARegion = songABlocks * 512
songBRegion = songBBlocks * 512
```

and, for the slot that would receive the next upload (the **inactive** one),
how much of its region a new song may use. A song must fit entirely inside
**one** region.

**The consequence to surface plainly:** because the two song regions split the
device evenly and an upload always targets the inactive one, **the largest
song the SP-1 can hold is about half the device's capacity.** The other half
is permanently reserved for the rollback copy. Users will otherwise read
"32 GB device" and expect a 32 GB song to fit. A single line near the capacity
figure is enough — something like "max song size: X (the other half is
reserved so an interrupted upload can roll back)".

### Please do NOT

- compute free space by subtracting both songs from `deviceBlocks` — that is
  not how the space is usable, and it will overstate what fits;
- offer reorder, delete, or "add another song" controls (see below);
- hide or fake a slot when it is empty — show it as empty. A fresh device
  legitimately has two empty slots and that should look normal.

### What to send back

The changed files, plus a screenshot of the view against a device (or your
mock) in three states: both slots empty, one song committed, and two slots
populated with different generations.

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
