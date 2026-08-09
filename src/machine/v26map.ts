/**
 * SP-1 TAPE LOOPER v2.6 — authoritative control map, transcribed verbatim from
 * chattock.github.io/sp1-tape-looper (the v2.6 reference card).
 *
 * Nothing in this table is invented. `command` is the documented wording.
 * `status` says how faithfully the browser twin can reproduce the row:
 *   "state"  — fully simulated in the state machine and observable
 *   "visual" — reproduced as lights / labels only
 *   "audio"  — needs the (Phase 4) audio engine to be audible; state is tracked
 *   "doc"    — documentation row, no interactive behaviour
 *
 * Phase 3 covers this map only. No experimental Stem Tape mappings are added
 * until every row below is implemented and testable.
 */

export type V26Status = "state" | "visual" | "audio" | "doc";

export interface V26Row {
  id: string;
  group: "rocker" | "volume" | "faders" | "track" | "play" | "function" | "heads" | "lights" | "songs";
  input: string;
  command: string;
  status: V26Status;
}

export const V26_MAP: V26Row[] = [
  // ROCKER — tape speed
  { id: "rocker.speed", group: "rocker", input: "fwd / rwd", command: "speed and pitch, ±1 BPM", status: "audio" },
  { id: "rocker.semitone", group: "rocker", input: "double-click", command: "exact semitone", status: "audio" },
  { id: "rocker.chop", group: "rocker", input: "FN + fwd / rwd", command: "chop half / double · hold = glide", status: "state" },
  { id: "rocker.chopReset", group: "rocker", input: "FN + dbl-click", command: "chop reset", status: "state" },

  // VOLUME
  { id: "volume.master", group: "volume", input: "vol − +", command: "master volume", status: "state" },
  { id: "volume.chopWindow", group: "volume", input: "FN + vol", command: "slide the chop window · hold = glide", status: "state" },

  // FADERS
  { id: "fader.trackVolume", group: "faders", input: "1–4", command: "track volumes · per head in heads mode", status: "state" },
  { id: "fader.window", group: "faders", input: "FN + 1 · 2 · 3", command: "window start · end · shift, free sizes", status: "state" },
  { id: "fader.windowReverse", group: "faders", input: "FN + 1 past 2", command: "the window plays in reverse", status: "state" },
  { id: "fader.filter", group: "faders", input: "FN + 4", command: "filter: mid = off · down LP · up HP", status: "state" },
  { id: "fader.headScrub", group: "faders", input: "in heads", command: "faders scrub the heads", status: "state" },

  // TRACK BUTTONS
  { id: "track.record", group: "track", input: "hold", command: "record — starts on your first sound", status: "state" },
  { id: "track.tap", group: "track", input: "tap", command: "stop the take · mute / unmute", status: "state" },
  { id: "loop.capture", group: "track", input: "double-tap", command: "capture one-bar loop (delete removed)", status: "state" },
  { id: "track.bank", group: "track", input: "FN + track", command: "banks: jump · tap again = next song", status: "state" },

  // PLAY
  { id: "play.toggle", group: "play", input: "tap", command: "play / stop", status: "state" },
  { id: "play.restart", group: "play", input: "hold", command: "restart from the top", status: "state" },
  { id: "play.loopMode", group: "play", input: "FN + play, release together", command: "fixed / variable loops", status: "state" },
  { id: "play.snap", group: "play", input: "FN + tap ×2", command: "snap to 1.0×", status: "state" },
  { id: "play.heads", group: "play", input: "FN + tap ×3", command: "heads mode", status: "state" },
  { id: "play.lights", group: "play", input: "hold thru 5 s", command: "dim / full lights", status: "visual" },

  // FUNCTION
  { id: "fn.power", group: "function", input: "hold", command: "power on / off", status: "state" },
  { id: "fn.tempoGrid", group: "function", input: "tap ×4 in rhythm", command: "the song gets a tempo grid", status: "state" },
  { id: "fn.beatmatch", group: "function", input: "re-tap over loops", command: "beatmatch to your taps", status: "state" },
  { id: "fn.clearGrid", group: "function", input: "tap, then quick hold", command: "clear the grid", status: "state" },
  { id: "fn.roundBpm", group: "function", input: "tap ×4, then hold", command: "round to the nearest whole BPM", status: "state" },
  { id: "fn.gridReject", group: "function", input: "unless it's far off", command: "all four blink, nothing moves", status: "visual" },

  // HEADS MODE
  { id: "heads.toggle", group: "heads", input: "FN + triple-tap PLAY", command: "heads on / off", status: "state" },
  { id: "heads.replay", group: "heads", input: "heads on", command: "3 tracks replay the source, a quarter apart", status: "audio" },
  { id: "heads.scrub", group: "heads", input: "FN + faders · double-tap", command: "scrub the heads · double-tap = reverse", status: "state" },

  // LIGHTS / SONGS (documentation rows the LED arbitration implements)
  { id: "lights.base", group: "lights", input: "track lights", command: "dark = empty · faint = muted content", status: "visual" },
  { id: "lights.pulse", group: "lights", input: "playing", command: "each light pulses as its own loop wraps (polyrhythm)", status: "visual" },
  { id: "lights.songRow", group: "lights", input: "song row", command: "solid = song · blink = bank", status: "visual" },
  { id: "songs.memory", group: "songs", input: "16 songs", command: "every song remembers loops, speed, chop, mutes, grid", status: "state" },
  { id: "songs.length", group: "songs", input: "takes", command: "to 8:00 · longer with the tape slowed", status: "doc" },
  { id: "songs.transfer", group: "songs", input: "transfer page", command: "WAVs in + out", status: "doc" },
];

export const V26_GROUP_LABEL: Record<V26Row["group"], string> = {
  rocker: "rocker — tape speed",
  volume: "volume",
  faders: "faders",
  track: "track buttons",
  play: "play",
  function: "function",
  heads: "heads mode",
  lights: "lights",
  songs: "16 songs + transfer",
};

export const V26_ROW_BY_ID: Record<string, V26Row> = Object.fromEntries(V26_MAP.map((r) => [r.id, r]));
