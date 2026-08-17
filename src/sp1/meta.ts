/**
 * SP-1 index (meta) block — parse/build.
 *
 * Layout copied from the proven transfer implementation:
 *   magic u32 | cur_slot u32 |
 *   slot[NUM_SLOTS] each 44 B: speed_q16 u32, loop_len u32,
 *                              present[4] u8, trk_len[4] u32, trk_start[4] u32
 *   fixed_len u32 | trk_content[NUM_SLOTS][4] u32 | settings u32 | chop/mode tail
 *
 * There is NO title, artist, BPM or downbeat field anywhere in this block.
 * Nothing is invented here: unknown tail bytes are preserved verbatim.
 */

import { BLOCK_BYTES, SAMPLES_PER_BLOCK, le32, put32, type Sp1Layout } from "./protocol";

export const SLOT_SZ = 44;
export const DEFAULT_SPEED_Q16 = 65536;

export interface Sp1Slot {
  speed: number;
  loopLen: number;
  present: number[];
  trkLen: number[];
  trkStart: number[];
  trkContent: number[];
}

export interface Sp1Meta {
  magic: number;
  cur: number;
  slots: Sp1Slot[];
  /** The device's own block, kept so unmanaged fields survive a rebuild. */
  raw: Uint8Array;
}

export function metaBlockCount(layout: Sp1Layout): number {
  return layout.numSlots > 8 ? 2 : 1;
}

function contentOffset(layout: Sp1Layout): number {
  return 8 + layout.numSlots * SLOT_SZ + 4;
}

export function parseMeta(b: Uint8Array, layout: Sp1Layout): Sp1Meta {
  const co = contentOffset(layout);
  const meta: Sp1Meta = { magic: le32(b, 0), cur: le32(b, 4), slots: [], raw: b.slice(0) };
  for (let s = 0; s < layout.numSlots; s++) {
    const o = 8 + s * SLOT_SZ;
    const slot: Sp1Slot = {
      speed: le32(b, o),
      loopLen: le32(b, o + 4),
      present: [],
      trkLen: [],
      trkStart: [],
      trkContent: [],
    };
    for (let t = 0; t < 4; t++) slot.present[t] = b[o + 8 + t]!;
    for (let t = 0; t < 4; t++) slot.trkLen[t] = le32(b, o + 12 + t * 4);
    for (let t = 0; t < 4; t++) slot.trkStart[t] = le32(b, o + 28 + t * 4);
    for (let t = 0; t < 4; t++) slot.trkContent[t] = le32(b, co + (s * 4 + t) * 4);
    meta.slots.push(slot);
  }
  return meta;
}

export function buildMeta(m: Sp1Meta, layout: Sp1Layout): Uint8Array {
  const size = metaBlockCount(layout) * BLOCK_BYTES;
  const b = m.raw && m.raw.length >= size ? Uint8Array.from(m.raw) : new Uint8Array(size);
  put32(b, 0, layout.magic);
  put32(b, 4, m.cur);
  const co = contentOffset(layout);
  for (let s = 0; s < layout.numSlots; s++) {
    const o = 8 + s * SLOT_SZ;
    const slot = m.slots[s]!;
    put32(b, o, slot.speed || DEFAULT_SPEED_Q16);
    put32(b, o + 4, slot.loopLen);
    for (let t = 0; t < 4; t++) b[o + 8 + t] = slot.present[t] ? 1 : 0;
    for (let t = 0; t < 4; t++) put32(b, o + 12 + t * 4, slot.trkLen[t]!);
    for (let t = 0; t < 4; t++) put32(b, o + 28 + t * 4, slot.trkStart[t]!);
    for (let t = 0; t < 4; t++) put32(b, co + (s * 4 + t) * 4, slot.trkContent?.[t] || 0);
  }
  return b;
}

export function slotIsOccupied(slot: Sp1Slot): boolean {
  return slot.present.some((p) => !!p);
}

/** Audible length of a track in blocks (trk_content wins when shorter). */
export function trackAudioBlocks(slot: Sp1Slot, t: number): number {
  if (!slot.present[t]) return 0;
  const base = slot.loopLen ? slot.loopLen / SAMPLES_PER_BLOCK : 0;
  const blocks = slot.trkLen[t] || base;
  const content = slot.trkContent[t] || 0;
  return content && content < blocks ? content : blocks;
}

export function blocksToSeconds(blocks: number, sampleRate: number): number {
  return (blocks * SAMPLES_PER_BLOCK) / sampleRate;
}

/** Device capacity, from the ping layout only. */
export function capacity(layout: Sp1Layout, meta: Sp1Meta) {
  const perTrackBlocks = layout.trackBlocks;
  const totalBlocks = layout.numSlots * layout.ntrk * perTrackBlocks;
  let usedBlocks = 0;
  for (const slot of meta.slots) for (let t = 0; t < 4; t++) usedBlocks += slot.present[t] ? slot.trkLen[t] || 0 : 0;
  return {
    totalBlocks,
    usedBlocks,
    freeBlocks: Math.max(0, totalBlocks - usedBlocks),
    perTrackBlocks,
    perTrackSeconds: blocksToSeconds(perTrackBlocks, layout.sampleRate),
  };
}
