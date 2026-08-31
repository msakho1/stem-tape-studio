# Stem Tape v1.3 — a true multi-song library

**Status: specified, deliberately not started.** v1.2 planar playback and
per-track reverse must be proven on hardware first. Two unverified storage
migrations in flight at once is how a device ends up with a fault nobody can
attribute.

This document exists so the requirement is recorded while it is fresh, and so
the hard part is not re-derived later. It is a statement of what v1.3 must do
and the one structural insight that makes it affordable — not a wire spec.

## The requirement, from the product owner

> One-song-plus-rollback is not the final Stem Tape architecture.

v1.3 must support:

- multiple songs, up to the available storage
- a device-side song index / catalog
- next / previous song selection **from the SP-1 itself**
- the companion listing all songs actually on the device
- uploading a new song **without destroying unrelated songs**
- deleting individual songs
- reordering the playlist / song order
- storage used / free reporting
- safe interrupted-upload behaviour **without reserving half the entire device
  for one rollback copy**

And, explicitly:

> The current A/B rollback safety is valuable, so when we design v1.3 I don't
> want to simply throw that protection away.

So the goal is not "drop the safety to get multi-song". It is: keep atomic,
crash-safe metadata updates and crash-safe uploads, and stop paying 50% of the
device for them.

## Why v1.1/v1.2 costs half the device, exactly

The A/B scheme mirrors **the whole song region**. `st11_storage_layout_compute()`
splits everything after the two index blocks into two equal song regions, and
`st_ab_session_open_replace()` freezes the inactive one as the only writable
destination. The previous song stays intact until the new one is verified and
the index commit switches which region is active.

That is genuinely good: the commit is a single 512-byte block write, so it is
atomic against power loss, and a torn or failed upload leaves the old song
playable. 146 injection points, five recorded wire transcripts and 1,186
byte-exact assertions all rest on it.

The cost is structural, not incidental: **the unit of rollback is the audio.**
Mirroring audio means mirroring the largest thing on the device.

## The insight that dissolves the cost

In a multi-song library, the thing that must change atomically is **the
catalog**, not the audio.

- A **catalog** is small — tens of bytes per song, a few blocks total. Keeping
  two copies of it and committing by generation is the same A/B trick the
  index blocks already use, at a fixed, negligible cost.
- **Audio is immutable once written.** A new song's blocks go into free space
  and are simply *not referenced by the catalog* until the upload has been
  verified. Nothing existing is overwritten, so nothing existing needs a
  mirror.

That inverts the failure model in the right direction:

| | v1.1/v1.2 | v1.3 |
|---|---|---|
| rollback unit | the whole song region | the catalog (a few blocks) |
| cost of safety | 50% of the device | ~2 catalog copies |
| interrupted upload | old song survives in the mirror | new blocks are never referenced; old songs untouched |
| cleanup needed | none | reclaim unreferenced blocks |

An interrupted upload leaves **orphaned blocks**, not a corrupted library. The
price of the design is that something must reclaim them — a free-space sweep
derived from the catalog, which is a bounded, restartable, offline job rather
than anything on the audio path.

The A/B generation-counter commit is therefore **kept and reused**, applied to
the catalog instead of to the audio. The safety property is preserved; only
what it protects gets smaller.

## The questions to settle when it starts

Recorded so the design begins from them rather than discovering them:

1. **Allocation.** Songs are variable length and delete makes holes. Fixed
   extents, a block bitmap, or contiguous-with-compaction? The read path wants
   contiguity — `st_pl_group_block()` assumes a song's four stem quarters are
   contiguous from `song_start_block`, and per-track reverse's whole cost
   argument depends on one read per stem. Fragmentation is not free here; it
   is a change to the audio-path cost model.
2. **Catalog capacity.** How many songs, and is that bounded by a fixed
   catalog size or by storage? A fixed cap keeps the catalog a known number of
   blocks, which keeps the atomic commit simple.
3. **Playlist order vs. catalog order.** Reordering must not move audio. The
   order should be a field in the catalog, not the physical layout.
4. **Device-side next/previous.** New: the SP-1 changes songs on its own, so
   song selection becomes runtime state the audio thread must observe safely —
   this touches the reload path (`g_stem_reload_req`) rather than storage.
5. **Reclaiming orphans.** When does the sweep run, and how does it prove a
   block is unreferenced? It must be safe to interrupt too.
6. **Migration.** A v1.2 device holds one song. v1.3 must either import it or
   require re-upload; the version gates already refuse across a mismatch, so
   the decision is about user experience, not safety.

## What v1.3 does NOT change

The planar audio format itself. v1.2 song-planar is about the layout *within*
a song, and per-track reverse depends on it. A song is still four contiguous
per-stem quarters with 2048-byte groups; v1.3 changes where songs sit relative
to each other and how they are catalogued, not how one song is read.
