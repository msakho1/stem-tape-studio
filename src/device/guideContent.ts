/**
 * Guide content model.
 *
 * The Guide is deliberately NOT eighty cards. It is:
 *
 *   - 20 animated performance lessons (the moves you make with your hands)
 *   - 4 FX bank accordions carrying all 12 algorithms
 *   - 1 complete keyboard table
 *   - compact Projects / Loading / Session / System reference sections
 *   - 1 hardware-only reference section (documented, not interactive)
 *
 * Coverage is tracked by FEATURE ID, not by card count: `FEATURE_IDS` lists all
 * eighty documented capabilities and `guideCoverage()` reports which of them a
 * rendered surface accounts for. A single lesson may cover several features.
 */

import type { Control } from "@/device/geometry";
import type { MiniMotion } from "@/device/Sp1GuideIllustration";
import { BANKS } from "@/machine/fx12";
import { keyboardBindings, type KeyBinding } from "@/device/keyboardMap";

export type GuideSection =
  | "performance"
  | "fx"
  | "keyboard"
  | "projects"
  | "loading"
  | "session"
  | "system"
  | "hardware";

export interface Feature {
  id: string;
  section: GuideSection;
  label: string;
  /** Reference sections render this line verbatim; lessons carry their own copy. */
  note: string;
}

/* ------------------------------------------------------------------ */
/* Performance features (41) — covered by the 21 animated lessons.     */
/* ------------------------------------------------------------------ */

const PERFORMANCE: [string, string][] = [
  ["transport.play", "Start the tape over its inertia ramp"],
  ["transport.stop.inertia", "Wind the tape down and park it"],
  ["lane.mute", "Mute one stem"],
  ["lane.unmute", "Unmute one stem"],
  ["lane.audition", "Momentary solo while a Track button is held"],
  ["lane.loop.capture", "Capture a one-bar loop on a stem"],
  ["lane.loop.release", "Release a stem loop back to the song"],
  ["lane.loop.resize", "Halve or double a stem loop"],
  ["lane.scrub.fader", "Scrub one stem audibly with its fader"],
  ["lane.scrub.park", "Park a stem where the scrub was released"],
  ["lane.scrub.candidate", "Capture a bar from the parked scrub point"],
  ["transport.scrub.global", "Shuttle all four stems together"],
  ["transport.scrub.landing", "Land cleanly when the shuttle is released"],
  ["transport.scrub.speed", "Four persistent shuttle speeds"],
  ["transport.scrub.latch", "Latch the shuttle so both hands are free"],
  ["transport.scrub.inertia", "The shuttle slows like tape when you let go"],
  ["lane.reverse", "Play one stem backwards"],
  ["lane.reverse.rejoin", "Rejoin the song when reverse is turned off"],
  ["varispeed.rocker", "Drag or run the tape fast with the rocker"],
  ["varispeed.reset", "Snap the tape speed back to 1.00x"],
  ["fx.overlay.open", "Open the FX overlay"],
  ["loop.global.hold", "Hold PLAY for one shared bar-locked loop"],
  ["loop.global.latch", "Latch the global loop so PLAY can be released"],
  ["loop.global.move", "Step the global loop window with the rocker"],
  ["loop.global.division", "Set the global loop division"],
  ["stem.solo.latch", "Latch a stem solo with PLAY + Track"],
  ["stem.link", "Link or unlink a stem with a longer PLAY + Track hold"],
  ["stem.select.armed", "Arm active-stem selection with a FUNCTION tap"],
  ["speed.half", "Fall to half speed"],
  ["speed.snap", "Snap the speed exactly to 1.000x"],
  ["transport.cue.stopped", "Cue every stem to the top while parked"],
  ["instrument.learn.global", "Learn a whole-song cue onto a MIDI key"],
  ["instrument.learn.isolated", "Learn a single-stem cue onto a MIDI key"],
  ["instrument.play", "Hold a learned cue to play it, release to return"],
  ["instrument.rejoin", "Rejoin the underlying tape when a cue ends"],
  ["song.skip.stopped", "Skip songs with the bare rocker while stopped"],
  ["heads.enter", "Enter Heads mode"],
  ["heads.source", "Four heads read the last selected stem"],
  ["heads.mute", "Mute one head"],
  ["heads.audition", "Audition one head"],
  ["heads.latch", "Latch a head playing"],
  ["heads.scrub", "Scrub one head"],
  ["heads.level", "Set one head's level with its fader"],
  ["heads.exit.restore", "Leaving Heads restores the mix exactly"],
];

/* ------------------------------------------------------------------ */
/* FX features (19) — four bank accordions, twelve algorithms.         */
/* ------------------------------------------------------------------ */

export interface FxBankCard {
  bankId: string;
  label: string;
  button: number;
  features: string[];
  algorithms: { id: string; label: string; blurb: string }[];
}

const ALGO_BLURB: Record<string, string> = {
  filter: "One sweep from dark low-pass to thin high-pass.",
  exciter: "Adds presence and upper harmonics, level-compensated — never a filter sweep.",
  dirt: "Saturation into bit/sample crush as the macro climbs.",
  reelFlange: "Tape flange from a second head running slightly out of step.",
  formantShift: "Moves vowel character without moving pitch.",
  gate: "Chops the stem into a grid-locked rhythmic gate.",
  echo: "Tempo-locked echo that follows the detected grid.",
  pitchEcho: "Echo whose repeats climb or fall in pitch.",
  scatter: "Granular scatter of the last fraction of a bar.",
  reverb: "Room to hall, wet amount on the macro.",
  shimmer: "Reverb with an octave-up feedback path.",
  freeze: "Freezes the spectrum into a sustained pad.",
};

export const FX_BANK_CARDS: FxBankCard[] = BANKS.map((b) => ({
  bankId: b.id,
  label: b.label,
  button: b.buttonIndex + 1,
  features: [`fx.bank.${b.id}`, ...b.algorithms.map((a) => `fx.algo.${a.id}`)],
  algorithms: b.algorithms.map((a) => ({ id: a.id, label: a.label, blurb: ALGO_BLURB[a.id] ?? "" })),
}));

/** Overlay grammar shared by every bank. */
export const FX_GRAMMAR_FEATURES = ["fx.momentary", "fx.latch", "fx.latch.feedback", "fx.cycle"];

const FX: [string, string][] = [
  ...BANKS.map((b) => [`fx.bank.${b.id}`, `${b.label} bank — Track ${b.buttonIndex + 1} in the FX overlay`] as [string, string]),
  ...BANKS.flatMap((b) => b.algorithms.map((a) => [`fx.algo.${a.id}`, a.label] as [string, string])),
  ["fx.momentary", "Hold a Track button for momentary FX"],
  ["fx.latch", "FUNCTION + Track latches the bank on"],
  ["fx.latch.feedback", "All four Track LEDs pulse when a bank latches or unlatches"],
  ["fx.cycle", "VOL -/+ cycles the algorithm inside the selected bank"],
];

/* ------------------------------------------------------------------ */
/* Keyboard features (6) — one complete table.                         */
/* ------------------------------------------------------------------ */

export interface KeyboardGroup {
  feature: string;
  label: string;
  match: (b: KeyBinding) => boolean;
}

export const KEYBOARD_GROUPS: KeyboardGroup[] = [
  { feature: "keyboard.transport", label: "transport", match: (b) => b.codes.includes("Space") },
  {
    feature: "keyboard.function",
    label: "function qualifier",
    match: (b) => b.codes.includes("KeyF") || b.label.startsWith("F +"),
  },
  { feature: "keyboard.tracks", label: "track buttons", match: (b) => b.codes.some((c) => c.startsWith("Digit")) },
  { feature: "keyboard.faders", label: "faders", match: (b) => b.id.startsWith("fader.") },
  {
    feature: "keyboard.rocker",
    label: "rocker & volume",
    match: (b) => b.codes.some((c) => ["KeyQ", "KeyA", "Minus", "Equal"].includes(c)),
  },
  { feature: "keyboard.release", label: "safety release", match: (b) => b.codes.includes("Escape") },
];

export function keyboardTable(): { group: KeyboardGroup; rows: KeyBinding[] }[] {
  const all = keyboardBindings();
  const used = new Set<KeyBinding>();
  const out = KEYBOARD_GROUPS.map((group) => {
    const rows = all.filter((b) => !used.has(b) && group.match(b));
    rows.forEach((r) => used.add(r));
    return { group, rows };
  });
  return out;
}

const KEYBOARD: [string, string][] = KEYBOARD_GROUPS.map((g) => [g.feature, `Keyboard: ${g.label}`]);

/* ------------------------------------------------------------------ */
/* Compact reference sections (12) and hardware-only (6).              */
/* ------------------------------------------------------------------ */

const PROJECTS: [string, string][] = [
  ["projects.import", "Drop or pick four stems; each file is decoded once and audited."],
  ["projects.roles", "Roles are proposed automatically — vocals, drums, bass, instruments — and swap rather than collide."],
  ["projects.trash", "Deleted stems go to a recoverable project trash, never straight to nothing."],
  ["projects.export", "Export a .stemtape bundle with every original file extension preserved."],
];

const LOADING: [string, string][] = [
  ["loading.decode", "Header sniffing rejects a file before it can waste a decode."],
  ["loading.memory", "Decoded PCM is budgeted in MiB against a tunable ceiling."],
  ["loading.grid", "BPM, beat phase and bar are detected automatically on load — local, deterministic, no AI, nothing to set."],
];

const SESSION: [string, string][] = [
  ["session.persist", "The session is written locally as you work."],
  ["session.restore", "Reopening restores stems, mix and grid; the transport waits for PLAY."],
  ["session.privacy", "No network request ever contains your audio."],
];

const SYSTEM: [string, string][] = [
  ["system.diagnostics", "The Machine panel shows the live command stream and engine state."],
  ["system.coverage", "Mapping coverage counts which control-map rows this session has exercised."],
];

const HARDWARE: [string, string][] = [
  ["hardware.power", "Long-press POWER: physical SP-1 only."],
  ["hardware.pairing", "Bluetooth pairing chord: physical SP-1 only."],
  ["hardware.bluetooth", "Wireless audio output: physical SP-1 only."],
  ["hardware.usb", "USB mass-storage mode: physical SP-1 only."],
  ["hardware.storage", "On-device flash song storage: physical SP-1 only."],
  ["hardware.battery", "Battery gauge and charge LED: physical SP-1 only."],
];

export interface ReferenceSection {
  id: GuideSection;
  title: string;
  entries: Feature[];
}

function feats(section: GuideSection, rows: [string, string][]): Feature[] {
  return rows.map(([id, note]) => ({ id, section, label: note, note }));
}

export const FEATURES: Feature[] = [
  ...feats("performance", PERFORMANCE),
  ...feats("fx", FX),
  ...feats("keyboard", KEYBOARD),
  ...feats("projects", PROJECTS),
  ...feats("loading", LOADING),
  ...feats("session", SESSION),
  ...feats("system", SYSTEM),
  ...feats("hardware", HARDWARE),
];

export const FEATURE_IDS: string[] = FEATURES.map((f) => f.id);

export const REFERENCE_SECTIONS: ReferenceSection[] = (
  [
    ["projects", "projects"],
    ["loading", "loading"],
    ["session", "session"],
    ["system", "system"],
  ] as [GuideSection, string][]
).map(([id, title]) => ({ id, title, entries: FEATURES.filter((f) => f.section === id) }));

export const HARDWARE_SECTION: ReferenceSection = {
  id: "hardware",
  title: "hardware only — documented, not interactive",
  entries: FEATURES.filter((f) => f.section === "hardware"),
};

/* ------------------------------------------------------------------ */
/* The 20 animated performance lessons.                                */
/* ------------------------------------------------------------------ */

export interface Lesson {
  id: string;
  title: string;
  gesture: string;
  body: string;
  highlight: Control[];
  motion: MiniMotion;
  /** Controls held down for the whole gesture (drawn solid, never blinking). */
  held?: Control[];
  /** Feature ids this lesson accounts for. */
  features: string[];
}

export const LESSONS: Lesson[] = [
  {
    id: "play",
    title: "How to start and stop",
    gesture: "PLAY",
    body: "Press PLAY once. The tape spins up over its inertia ramp instead of snapping to speed. Press again to wind down and park.",
    highlight: ["play"],
    motion: "press",
    features: ["transport.play", "transport.stop.inertia"],
  },
  {
    id: "mute",
    title: "How to mute a stem",
    gesture: "Track 1–4 · single tap",
    body: "One tap on a Track button mutes that stem, another unmutes it. Single, double and triple taps are mutually exclusive — a double tap never flashes the mute first.",
    highlight: ["track-button-1"],
    motion: "press",
    features: ["lane.mute", "lane.unmute"],
  },
  {
    id: "audition",
    title: "How to solo momentarily",
    gesture: "Track 1–4 · hold",
    body: "Hold a Track button to hear only that stem for as long as you hold it. Hold two and you hear both. Release and the previous balance returns.",
    highlight: ["track-button-2"],
    motion: "hold",
    features: ["lane.audition"],
  },
  {
    id: "loop",
    title: "How to grab a one-bar loop",
    gesture: "Track 1–4 · double tap",
    body: "Double-tap a Track button to capture a one-bar loop on that stem, starting at the detected grid. Double-tap again to release it back to the song.",
    highlight: ["track-button-3"],
    motion: "double",
    features: ["lane.loop.capture", "lane.loop.release"],
  },
  {
    id: "scrub-lane",
    title: "How to scrub one stem",
    gesture: "FUNCTION + Fader 1–4",
    body: "Hold FUNCTION and move a fader: that stem scrubs audibly under your finger. Release at the sound you want — the stem parks there. Then double-tap its Track button to capture a bar from that point.",
    highlight: ["function", "fader-1"],
    motion: "fader",
    held: ["function"],
    features: ["lane.scrub.fader", "lane.scrub.park", "lane.scrub.candidate"],
  },
  {
    id: "scrub-all",
    title: "How to shuttle the whole song",
    gesture: "FUNCTION + rocker",
    body: "Hold FUNCTION and push the rocker to shuttle all four stems together, forwards or backwards. Release and playback lands cleanly at the new position.",
    highlight: ["function", "rocker-fwd", "rocker-rwd"],
    motion: "rocker",
    held: ["function"],
    features: ["transport.scrub.global", "transport.scrub.landing"],
  },
  {
    id: "scrub-speed",
    title: "How to change the shuttle speed",
    gesture: "FUNCTION + rocker, then VOL −/+",
    body: "While you are shuttling, VOL − and VOL + step through four shuttle speeds: 1.25×, 1.6×, 2.5× and 4×. The speed is remembered — the next shuttle, in either direction, starts at the speed you left it on.",
    highlight: ["function", "rocker-fwd", "volume-minus", "volume-plus"],
    motion: "sequence",
    held: ["function", "rocker-fwd"],
    features: ["transport.scrub.speed"],
  },
  {
    id: "scrub-latch",
    title: "How to latch the shuttle",
    gesture: "FUNCTION + rocker, then tap FUNCTION",
    body: "Tap FUNCTION while shuttling and the shuttle latches in that direction, so you can let both the rocker and FUNCTION go. Push the opposite side of the rocker to flip direction without dropping the latch. Tap FUNCTION again, or press PLAY, to let it go.",
    highlight: ["function", "rocker-fwd", "rocker-rwd"],
    motion: "sequence",
    features: ["transport.scrub.latch"],
  },
  {
    id: "scrub-inertia",
    title: "How the shuttle lets go",
    gesture: "Release the rocker",
    body: "Release and the shuttle does not stop dead: the reels slow over a real inertia curve back into the song, and a reverse shuttle passes through stillness before it turns around. Playback rejoins on the exact frame the deceleration ends, so the landing is silent.",
    highlight: ["rocker-fwd", "rocker-rwd"],
    motion: "rocker",
    features: ["transport.scrub.inertia"],
  },
  {
    id: "reverse",
    title: "How to reverse a stem",
    gesture: "FUNCTION + Track · double tap",
    body: "Hold FUNCTION and double-tap a Track button to play that stem backwards — with or without a loop. Its hidden song position keeps moving, so turning reverse off rejoins the song where it now is.",
    highlight: ["function", "track-button-4"],
    motion: "double",
    held: ["function"],
    features: ["lane.reverse", "lane.reverse.rejoin"],
  },
  {
    id: "resize",
    title: "How to resize a loop",
    gesture: "FUNCTION + Track + VOL −/+",
    body: "Hold FUNCTION and the Track button, then press VOL − or VOL + to halve or double that lane's loop length.",
    highlight: ["function", "track-button-1", "volume-minus", "volume-plus"],
    motion: "sequence",
    held: ["function", "track-button-1"],
    features: ["lane.loop.resize"],
  },
  {
    id: "varispeed",
    title: "How to change tape speed",
    gesture: "Rocker",
    body: "Push the rocker without FUNCTION for varispeed: the tape drags or runs fast, pitch and all. Double-tap it to snap back to 1.00×.",
    highlight: ["rocker-fwd", "rocker-rwd"],
    motion: "rocker-click",
    features: ["varispeed.rocker", "varispeed.reset"],
  },
  {
    id: "fx",
    title: "How to use FX",
    gesture: "VOL − + VOL + together",
    body: "Press both volume buttons together to open FX on the selected stem (STEM scope). Hold FUNCTION first, then press both volume buttons, and FX opens on the whole mix instead (GLOBAL scope) — one rack over everything you hear, loops and MIDI cues included. Inside STEM scope, FUNCTION + VOL − / VOL + walks the target stem. Each Track button is a bank — TONE, MOD, MOTION, SPACE — held for momentary, FUNCTION + Track to latch. VOL −/+ cycles the algorithm inside the selected bank. Pressing both volume buttons again closes FX.",
    highlight: [
      "volume-minus",
      "volume-plus",
      "track-button-1",
      "track-button-2",
      "track-button-3",
      "track-button-4",
    ],
    motion: "fx",
    features: ["fx.overlay.open", ...FX_GRAMMAR_FEATURES],
  },
  {
    id: "global-loop",
    title: "How to loop the whole song",
    gesture: "Hold PLAY (while playing)",
    body: "Hold PLAY while the song runs and all four stems drop into one shared bar-locked loop. Release PLAY and the song continues from where it would have been. Nothing drifts: it is one window, not four.",
    highlight: ["play"],
    motion: "hold",
    features: ["loop.global.hold"],
  },
  {
    id: "global-loop-latch",
    title: "How to keep the global loop running",
    gesture: "Hold PLAY, then tap FUNCTION",
    body: "While the global loop is held, tap FUNCTION to latch it so you can let PLAY go. Tap FUNCTION again, or press PLAY, to drop back into the song.",
    highlight: ["play", "function"],
    motion: "sequence",
    held: ["play"],
    features: ["loop.global.latch"],
  },
  {
    id: "global-loop-move",
    title: "How to move the global loop",
    gesture: "Hold PLAY + rocker",
    body: "With the global loop held, push the rocker to step the loop window forward or back one division at a time. The transport never toggles — PLAY is owned by the loop.",
    highlight: ["play", "rocker-fwd", "rocker-rwd"],
    motion: "rocker",
    held: ["play"],
    features: ["loop.global.move"],
  },
  {
    id: "global-loop-division",
    title: "How to change the loop length",
    gesture: "FUNCTION + VOL −/+",
    body: "FUNCTION + VOL −/+ is contextual, exactly like the stock SP-1. While a global loop is captured or latched it sets the loop division: one bar, half, quarter, eighth — change it while the loop runs and the window resizes from the same start. While the shuttle is running it steps the shuttle speed instead. With neither running it walks the active stem.",
    highlight: ["function", "volume-minus", "volume-plus"],
    motion: "sequence",
    held: ["function"],
    features: ["loop.global.division"],
  },
  {
    id: "solo",
    title: "How to latch a solo",
    gesture: "PLAY + Track (release under 0.7 s)",
    body: "Press PLAY, then a Track button within 0.45 s, and let go: that stem latches solo. The chord takes ownership of PLAY, so no transport and no global loop can fire from the same press.",
    highlight: ["play", "track-button-1"],
    motion: "sequence",
    held: ["play"],
    features: ["stem.solo.latch"],
  },
  {
    id: "link",
    title: "How to link stems",
    gesture: "PLAY + Track (hold past 0.7 s)",
    body: "Same chord, held longer: keep PLAY and the Track button down past 0.7 s to link or unlink that stem. Linked stems take tape moves as a group, phase-continuous — nothing restarts.",
    highlight: ["play", "track-button-2"],
    motion: "hold",
    held: ["play"],
    features: ["stem.link"],
  },
  {
    id: "select",
    title: "How to choose the active stem",
    gesture: "FUNCTION tap, then a Track button",
    body: "With no global loop running, a FUNCTION tap arms selection for a moment. The next Track button becomes the active stem — it only selects, it will not mute, loop or audition.",
    highlight: ["function", "track-button-3"],
    motion: "sequence",
    features: ["stem.select.armed"],
  },
  {
    id: "halfspeed",
    title: "How to drop to half speed",
    gesture: "FUNCTION + PLAY · single tap",
    body: "Hold FUNCTION and tap PLAY once to fall to 0.5×, again to return to 1.00×. Double-tap snaps the speed exactly to 1.000× from anywhere.",
    highlight: ["function", "play"],
    motion: "press",
    held: ["function"],
    features: ["speed.half", "speed.snap"],
  },
  {
    id: "cue",
    title: "How to cue and skip while stopped",
    gesture: "Hold PLAY stopped · rocker stopped",
    body: "Hold PLAY while the tape is parked to cue every stem to the top. With the tape stopped the bare rocker skips songs instead of changing speed.",
    highlight: ["play", "rocker-fwd", "rocker-rwd"],
    motion: "sequence",
    features: ["transport.cue.stopped", "song.skip.stopped"],
  },
  {
    id: "instrument",
    title: "Play Stem Tape with MIDI",
    gesture: "FUNCTION + MIDI key · Track + MIDI key · MIDI key",
    body: `Save part of the whole song
Hold FUNCTION and hold any MIDI key while the part you want plays. Release the key when the part ends. That key now holds that section of the song.

Save just one stem
Hold its TRACK button instead of FUNCTION, then do the same thing.

Play it
Hold a learned pad to play its cue. Release it to return. A whole-song cue parks the tape; a stem cue lets the other three stems continue.

Each pad needs a unique note, channel, or combination. If your pads transmit the same note, assign them different channels.

“MIDI works in supported browsers such as desktop Chrome and Edge. It is not currently available in iPhone or iPad browsers.”`,
    highlight: ["function", "track-button-1", "play"],
    motion: "sequence",
    held: [],
    features: [
      "instrument.learn.global",
      "instrument.learn.isolated",
      "instrument.play",
      "instrument.rejoin",
    ],
  },
  {
    id: "heads",
    title: "How to use Heads mode",
    gesture: "FUNCTION + PLAY · triple tap",
    body: "Hold FUNCTION and triple-tap PLAY. Four independent heads read the SAME stem — the one you last selected — at four positions in the song. Track 1–4 mute their head, hold to audition, triple-tap to latch it playing, FUNCTION + fader scrubs that head, and the fader alone is its level. Leaving Heads restores the mix exactly as it was.",
    highlight: ["function", "play"],
    motion: "triple",
    held: ["function"],
    features: [
      "heads.enter",
      "heads.source",
      "heads.mute",
      "heads.audition",
      "heads.latch",
      "heads.scrub",
      "heads.level",
      "heads.exit.restore",
    ],
  },
];

/* ------------------------------------------------------------------ */
/* Coverage.                                                           */
/* ------------------------------------------------------------------ */

/** Every feature id the rendered Guide accounts for, from all surfaces. */
export function guideCoverage(): Set<string> {
  const out = new Set<string>();
  for (const l of LESSONS) l.features.forEach((f) => out.add(f));
  for (const b of FX_BANK_CARDS) b.features.forEach((f) => out.add(f));
  FX_GRAMMAR_FEATURES.forEach((f) => out.add(f));
  for (const { group, rows } of keyboardTable()) if (rows.length > 0) out.add(group.feature);
  for (const s of [...REFERENCE_SECTIONS, HARDWARE_SECTION]) s.entries.forEach((e) => out.add(e.id));
  return out;
}
