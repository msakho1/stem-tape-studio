# The end of a song — the contract (st62, Stage 2A-i)

**A song that reaches its end stops, and stays stopped, until somebody presses
PLAY.** That is the whole of this change. Nothing else in the instrument moves.

---

## 1. What was wrong

Every playback stream was created with `loop_enabled = true`. So
`st_stream_advance_frames()` answered "the last frame was consumed" by setting
`song_frame = 0` and returning `ST_STREAM_TICK_LOOPED`, and `main.c` had no
handler for `ST_STREAM_TICK_ENDED` anywhere — it could not be produced.

On hardware: the song reached its end and **started again with nobody pressing
PLAY**. `g_playing` was still set, the reel was still turning, all four heads
jumped to frame 0 with residency invalidated, and four asynchronous re-primes
raced the streamer — which is also a fresh opportunity for the heads to come
back audibly displaced from each other.

## 2. The two things that must never be confused

| | `st_stream_t::loop_enabled` | the Stem Tape loop |
|---|---|---|
| what it is | the **whole-song wrap** inside the streaming state machine | the user-facing loop: FUNCTION-held latch, window, division |
| where it lives | `st_stem_stream.c` | `st_loop.c` + `main.c`'s window code |
| how it wraps | `song_frame = 0` at the end of the song | `st_stream_seek()` inside `[loop_start, loop_end)` |
| st62 | **off** | **unchanged** |

`st_loop.c` and `st_loop.h` do not contain the string `loop_enabled`. Gate F-3
in `stemtape_player_stem_playback_wiring_check.py` asserts that, so "turning the
whole-song wrap off cannot reach loop entry, wrap or release" is a checked fact
rather than an assurance.

## 3. The authoritative end-of-song event

> Within **one audio block**, after all four heads have been advanced and after
> the loop-window backstop has run, the transport is at end of song **iff**
>
> * **(a)** no user loop window is latched for this block (`lp_on == false`), **and**
> * **(b)** at least one of the four heads returned `ST_STREAM_TICK_ENDED` from
>   its advance in this block.
>
> The **first** such head, whichever lane it is, is the authoritative event.

**Why the first head and not the transport head.** The transport head can be the
starved one — a head that is not resident does not advance, so a lane that is
ahead can reach the end while the transport is still short of it. Waiting for
the transport would leave that lane parked at `END_OF_SONG` while the others
played on: one stem stopping while the rest continue. Taking the first is at
worst *slightly early*, and slightly early is the preferred error here.

**Why it cannot become four decisions.** The event is only *observed* per lane.
It is *applied* by `stem_streams_end_of_song()`, in one pass, in the same block,
**before another output frame is rendered** — the remainder of the block is
filled with silence and the run loop breaks. That call:

* parks all four heads at exactly `frames` (`st_stream_end_of_song()`), so all
  four agree on where the song ended;
* invalidates all four residencies;
* drops all four resamplers (`stem_rs_drop()`);
* cancels any pending seam jump;
* **stops the reel in that same block** (`st_inertia_reset()`);
* raises `g_stem_eof_req` once.

So there is no window in which three lanes have ended and one has not, no lane
seeks to zero early, no partial reset, and no re-prime race — nothing seeks and
nothing re-primes, because the heads are parked and the audio branch is gated
off until a deliberate PLAY.

**Why a latched loop is exempt.** A window whose end sits exactly on the song end
can make a head report `ENDED` before the backstop wraps it. Under the old flag
that head wrapped to frame 0 — *outside* the window, a real bug. Now the
backstop's own `st_stream_seek(hd, lp_lo)` lifts `END_OF_SONG` straight back to
`PLAYING` at `loop_start`, and condition (a) stops the transport being killed
underneath it. A latched loop never ends the song.

## 4. The handoff

The audio thread never writes `g_playing`; the control thread never touches the
heads. `g_stem_eof_req` crosses between them, in the same direction and with the
same discipline as every other `g_stem_*_req`.

```
audio  (block N)    park all four · reel to rest · g_stem_eof_req = 1
audio  (N+1, N+2…)  branch GATED OFF while the flag is set — falls through to
                    the ordinary "no stem song playing" tail: stem_streams_stop(),
                    st_inertia_reset(), stem_rs_drop(), meters dark
control (≤ ~8 ms)   g_playing = 0  ·  MIDI Stop  ·  then g_stem_eof_req = 0
```

**The order inside the control block matters.** `g_playing` is written *before*
the flag is cleared, so by the time the audio thread can observe the flag clear,
the transport request is already false. Clearing first would open a window in
which the audio thread sees "playing, not at EOF" and spins the reel back up.

**Why the reel is stopped by the audio thread and not only by the flag's
branch.** The flag is set by the audio thread and cleared by the control thread,
so the audio thread is not guaranteed to observe it set even once. If the reel
were still running at full envelope when the control thread cleared it, the next
block would call `st_inertia_stop()` and start a 600 ms spin-down — during which
`st_inertia_moving()` is true, the branch is entered anyway, the heads are found
parked at the song end and the replay rewind fires. That is the unsolicited
restart, rebuilt out of the spin-down. The host gate found exactly that during
development; `tests/test_song_end_gate.c` case 3 sweeps the two threads' relative
phase so it cannot come back.

**Why there is no spin-down.** A spin-down is audible, pitched audio *read from
the tape*. Past the last frame there is no tape to read it from. Stopping at
once is also what makes the 300 s idle clock start where the song actually ended
(`transport_active = g_playing || st_inertia_moving()`).

## 5. Manual replay

A PLAY that follows the end of the song is a **deliberate restart**. There is no
new request flag and no edge detector: the transport can only be parked at the
song end by the park above, and the branch can only be entered with it parked
there if something asked to play again — so reaching there in that position *is*
the press, whether it came from the button, the FUNCTION-qualified path or MIDI.

`stem_streams_rewind_to_start()` seeks all four heads to frame 0 with one
`st_stream_seek()` each, drops all four resamplers once, and resets the seam.
Four heads, frame 0, residency invalidated, interpolation state clear, eligible
to start on the same block. It does **not** touch `reverse` (an ordinary STOP
does not either), does not touch `s_stem_transport`, and does not touch the loop
window.

`END_OF_SONG` is deliberately **sticky**: `st_stream_play()` promotes only
`STOPPED → PLAYING`, so the transport's every-block re-assert of PLAY cannot
resurrect a finished song. Only an explicit position change gets a head out of
it, which is why the replay path seeks.

## 6. What proves it

| | |
|---|---|
| `tests/test_song_end_gate.c` | 15 cases, 256 checks. Real `st_stem_stream.c`, driven by a **model** of `main.c`'s wiring. Half of it is the loop regression wall. |
| `stemtape_player_song_end_gate_mutations.py` | 12 mutations, each removing one property of the design, each required to turn the gate **red**. |
| wiring check **F-1/F-2/F-3** | reads the **production** `main.c`: all three init call sites pass `false`; the tick, the shared park, the loop exemption and the replay rewind are all present; `st_loop.[ch]` never mention `loop_enabled`. |
| loop transport gate | still 0 entry seeks, 0 exit seeks; the seek count moves 3 → 4 and the fourth must be `stem_streams_rewind_to_start()` by name. |
| `test_stem_playback_gate.c` | deterministic playback hash `0x2a737e00`, unchanged. |

**What none of it proves.** The gate's transport is a *model* — `main.c` cannot
be linked on the host — so it can agree with a `main.c` that has drifted from
it. That is the reason F-1/F-2/F-3 read the production file directly.

---

## 7. OPEN — st62 did NOT stop on hardware

**Status: known, open, deliberately deferred.** Do not treat §1–§6 as a shipped
fix; treat them as the design, one path of which is still missing.

Hardware, st62, `44fad34`:

| | |
|---|---|
| ordinary playback synchronised | yes |
| song end audible click / pop / glitch | **none** |
| song stopped at its natural end | **NO — it still restarted automatically** |
| the unsolicited second pass | **synchronised** (it was not, before st62) |

Two of those are new information and both matter.

**The restart still happens**, so `ST_STREAM_TICK_ENDED` is reaching
`stem_streams_end_of_song()` on at least some runs — or never. The whole-song
wrap is provably off (gate F-1 reads all three call sites), so the head is
reaching `END_OF_SONG` and something downstream is restarting it. The only code
that can produce a frame-0 restart in st62 is `stem_streams_rewind_to_start()`,
reached from the replay-on-PLAY branch when `stem_streams_at_song_end()` is true
and `g_playing` is still set.

**The second pass is now synchronised**, which is corroborating evidence for
exactly that: the old whole-song wrap was four independent `song_frame = 0`
assignments with four independent residency invalidations, and it produced an
audibly displaced second pass. The st62 rewind is one coherent pass — four seeks
to frame 0, one `stem_rs_drop()`, one seam reset. The second pass being clean is
what that path would sound like.

### Leading hypothesis — NOT verified, no capture taken

`main.c`'s whole-block underrun path discards the tick result:

```c
for (sk = 0; sk < ST_PL_STEMS; sk++) {
        (void)st_stream_advance_frames(&g_stem_stream[sk], 1u);   /* main.c */
}
```

A head sitting on `frames - 1` that advances through **this** call returns
`ST_STREAM_TICK_ENDED` into a `(void)` cast. No `eof_seen`, no shared park, no
`g_stem_eof_req`, no `g_playing = 0` — but the head *is* now at `frames` in
`END_OF_SONG`, so the next block finds `stem_streams_at_song_end()` true with
`g_playing` still 1, and rewinds. A silent restart, through the new coherent
path. That matches both observations.

Why this path is likelier at the song end than anywhere else: read-ahead is
deliberately disabled there (`ahead = ST_STEM_GEOM.loop_enabled ? 0u :
needed[sk]` — with the wrap now off, the head re-publishes its current sector
and the ring stops filling forward), so residency is at its most fragile
precisely over the last sector. And during a starvation episode this path
advances *every* head by exactly one frame per block, so a starved run-out
arrives at `frames` through here rather than through the normal advance loop.

The obvious repair is to collect the tick here as well as in the main advance
loop. **It is not being made now, by instruction.** When it is, it needs the
same treatment as everything above — the model's underrun path must gain the
same hole first, so the mutation proves the gate would have caught it.

### What would settle it in one run

A serial capture over the last ~10 s of a song, printing per block: the four
`song_frame`s, `g_stem_eof_req`, `g_playing`, and
`g_stem_underrun_frames`. If the underrun counter climbs over the final sector
and the heads crawl to `frames` a frame at a time, the hypothesis is confirmed
and the fix is one line. If they arrive in a single 256-frame step and
`g_stem_eof_req` never rises, it is somewhere else and the hypothesis is wrong.

### The acceptance test, still unmet

> fresh boot → PLAY → let the song reach its natural end → it stops and stays
> stopped → press PLAY → it restarts deliberately, from the beginning.
