# The SP-1 device screen — build it once, expand it later

Supersedes `stem-tape-library-view-companion-handoff.md`, which described a
temporary developer view. This one is the real thing: final-quality UX, with a
data model shaped for the multi-song library in
`stem-tape-v1.3-multi-song-library.md`, so v1.3 **extends** this screen rather
than replacing it.

The rule that keeps both true at once:

> **Model the future. Surface only the present.**
> The data model is plural and general. The controls are exactly what the
> firmware can honour today, and no more.

---

## PROMPT FOR LOVABLE — copy everything below this line

Please build the SP-1 device-management screen properly. This replaces any
temporary or developer-facing device view.

Two constraints that pull in different directions, and both matter:

1. **Architect for a multi-song library.** The SP-1 will hold many songs, with
   delete, reorder and device-side next/previous. The data model, state shape
   and component structure must accommodate that now, so adding it later is
   extension rather than a rewrite.
2. **Only surface what the firmware can truly do today.** Today's firmware
   holds **one song**. Do not build delete, reorder, multi-select or "add
   another song" — not even disabled or greyed-out with a "coming soon"
   tooltip. A control that cannot work is worse than an absent one.

So: plural model, singular reality, and a screen that looks finished rather
than like a placeholder.

### The data model

Shape it like this (names indicative, structure is the point):

```ts
type DeviceConnection =
  | { status: 'disconnected' }
  | { status: 'connecting' }
  | { status: 'incompatible'; deviceFormat: string; appFormat: string }
  | { status: 'ready'; device: DeviceInfo };

interface DeviceInfo {
  firmwareId: number;
  protocol: { major: number; minor: number };
  format:   { major: number; minor: number };
  sampleRate: number;
  capabilities: {          // what THIS firmware supports; drives the UI
    multiSong: boolean;    // false on v1.2
    deleteSong: boolean;   // false on v1.2
    reorderSongs: boolean; // false on v1.2
  };
}

interface Song {
  id: string;              // stable key; v1.2 can derive it from the generation
  title: string;
  artist: string;
  durationSeconds: number; // frames / sampleRate
  sizeBytes: number;       // songBlockCount * 512
  isActive: boolean;
}

interface Storage {
  capacityBytes: number;   // total space songs may occupy
  usedBytes: number;       // sum of stored songs
  freeBytes: number;
  maxSongBytes: number;    // the largest single song that will fit
}

interface DeviceState {
  connection: DeviceConnection;
  songs: Song[];           // 0 or 1 entries on v1.2; many on v1.3
  activeSongId: string | null;
  storage: Storage;
  upload: UploadState;
}
```

**Why `maxSongBytes` is separate from `freeBytes`, and please keep it
separate.** On today's firmware they are *not* the same number, and on the
future firmware they mostly will be. Rendering one number where the meaning
differs is how this screen would come to lie later. Ask the device for both,
show "free" as the headline and `maxSongBytes` wherever the user is choosing a
file to upload.

**`capabilities` drives the UI, not the version number.** Please branch on
`capabilities.deleteSong` rather than on `format.minor === 2`. When the
firmware gains the feature, the same build starts showing the control. This is
the single most important thing for making v1.3 an extension.

**The rollback copy is not a Song.** Today's firmware keeps a second internal
copy of the previous song so a failed upload can roll back. It is not
playable, not chosen by the user, and must **never** appear in `songs[]`. Its
existence belongs only in the advanced section below.

### Where the data comes from

Enter transfer mode by sending the `SP1XFER!` magic, then:

| verb | use |
|---|---|
| `P` | ping — liveness |
| `Q` | capability reply (100 bytes: `STCP` tag + 96-byte body) |
| `R` | read one block — used to read the index records |
| `U` | bulk sector upload |
| `F` | durability barrier; also the commit trigger |
| `X` | exit transfer mode |

**From the `'Q'` body** (offsets into the 96 bytes after the tag, all
little-endian u32 unless noted):

| offset | field |
|---:|---|
| 8 | `formatMajor` (u16), 10 `formatMinor` (u16) |
| 32 | `deviceBlocks` |
| 36 / 40 | `songAStart` / `songABlocks` |
| 44 / 48 | `songBStart` / `songBBlocks` |
| 52 / 60 | `indexAStart` / `indexBStart` |
| 68 | `activeIndexSlot` — 0 = A, 1 = B, `0xffffffff` = none |
| 72 | `activeSongSlot` — same encoding |
| 76 / 80 | `activeGenerationLo` / `activeGenerationHi` |

**For the song itself**, `R`-read the block at `indexAStart` and at
`indexBStart` and parse both as STIX v2 records — you already do this
(`stemIndex.ts` / `activeIndex.ts`). The record named by `activeSongSlot`, if
valid and with `SONG_PRESENT` set, is the one song; map it to `songs[0]` with
`isActive: true`. Otherwise `songs` is empty.

**Deriving `Storage` on this firmware:**

```
capacityBytes = songABlocks * 512      // songABlocks === songBBlocks
usedBytes     = activeSong ? songBlockCount * 512 : 0
freeBytes     = capacityBytes - usedBytes
maxSongBytes  = capacityBytes          // a new song replaces, so it gets the whole region
```

Please **do not** add the two song regions together. A song cannot span them,
so the sum promises roughly twice what actually fits.

### Upload: show the phases, not one bar

An upload is not a single linear operation, and a progress bar that pretends
otherwise leaves users staring at a stalled-looking 100%. The real sequence:

```
connect → check compatibility → open session → send sectors → verify → commit → reload
```

```ts
type UploadState =
  | { phase: 'idle' }
  | { phase: 'preparing' }                                  // encoding locally
  | { phase: 'sending'; sectorsSent: number; sectorsTotal: number }
  | { phase: 'verifying' }                                  // device re-reads and checksums
  | { phase: 'committing' }                                 // the atomic moment
  | { phase: 'done' }
  | { phase: 'failed'; reason: UploadFailure; existingSongIntact: boolean };
```

- **sending** is the only phase with real determinate progress:
  `sectorsSent / sectorsTotal`. Each sector is written, read back off the
  media and CRC-checked by the device before it acks, so this progress is
  honest rather than "bytes handed to the port".
- **verifying** and **committing** are short but not instant. Give them their
  own indeterminate state with their own label rather than parking the bar at
  100%.
- **committing** is the atomic instant. Before it, the old song is still the
  active one; after it, the new one is.

### Failure is safe, and the UI should say so

This is the most valuable thing this screen can communicate. Every failure
mode before the commit leaves the existing song **completely intact and
playable** — the device is built around that guarantee.

So when an upload fails, lead with reassurance, then the cause:

> **Upload failed — your existing song is untouched.**
> The connection dropped after 412 of 1,204 chunks. Nothing on the SP-1 was
> changed. Try again when you're ready.

Distinct failures worth their own message:

| cause | what to say |
|---|---|
| version mismatch | The SP-1's firmware and this app expect different formats. Name both versions and say which needs updating. Do **not** present this as a generic connection error — it is the single most likely confusing failure right now. |
| song too large | Give the song's size and `maxSongBytes` in the same sentence. |
| connection lost mid-send | As above: existing song untouched. |
| verification failed | The data that arrived didn't match what was sent; the upload was rejected and the existing song is untouched. Offer retry. |
| device busy / not in transfer mode | Plain language: the SP-1 is playing. |

### Connection and device state

Make the connection state unambiguous at all times — the current app leaves
people guessing. Four states, each visually distinct:

- **Disconnected** — with the action that fixes it (connect the SP-1 by USB).
- **Connecting** — transient.
- **Incompatible** — the device answered but its format version doesn't match.
  Name both versions.
- **Ready** — show the device is connected and, quietly, its firmware version.

While in transfer mode the SP-1 pauses playback. If that's visible to the
user, say so plainly ("playback paused while transferring"), not as an error.

### Empty and loading states

- **No song on the device.** A real empty state — the device is fine, it just
  hasn't been given a song. Show capacity and the upload action. Do not show
  an error, a spinner, or a zeroed-out song card.
- **Reading the device.** A skeleton of the real layout, not a spinner in the
  middle of a blank screen.
- **No device.** The screen should still explain what it's for.

### The advanced / diagnostics section

Collapsed by default, off the normal path. This is where the internal storage
detail belongs, and it is genuinely useful when an upload misbehaves:

- both index slots, each with title, generation, and whether the record
  validates
- which slot is currently active
- region starts and block counts, device block count
- firmware id, protocol and format versions
- the last upload's failure detail, verbatim, copyable

**Outside this section the words "slot A", "slot B", "generation" and
"rollback" must not appear.**

### What NOT to build

- delete, reorder, multi-select, or "add another song" — in any form,
  including disabled
- anything that renders `songs.length` as though it could exceed 1 today
  (a list is fine architecturally; a UI implying you can add a second is not)
- summed capacity across both regions

### What to send back

The changed files, plus screenshots of: disconnected, connecting,
incompatible, ready-with-no-song, ready-with-a-song, mid-upload, upload
failed, and the advanced section expanded.

## END OF PROMPT

---

## Why the model is plural now

v1.3 (`stem-tape-v1.3-multi-song-library.md`) adds multiple songs, delete,
reorder, device-side next/previous and used/free reporting over a real pool.
Everything in the model above already accommodates that:

- `songs[]` goes from length 1 to length n
- `capabilities.*` flip to true and the corresponding controls appear
- `storage.freeBytes` becomes a real pool figure, and `maxSongBytes` stops
  being equal to `capacityBytes` — which is exactly why they are two fields
  rather than one

The one thing deliberately **not** modelled yet is playlist order. It is a
v1.3 catalog field, and inventing a client-side ordering now would create a
notion of order the device cannot store — the same mistake as a fake reorder
control, one layer down.
