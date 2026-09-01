/**
 * Capacity is a property of the DEVICE LAYOUT, never of occupancy.
 *
 * A song can never span the two song regions, so capacity must always be one
 * region (song A). The failure mode this guards is the empty-device path
 * reporting either the SUM of both regions or the whole device — which is
 * exactly 2x on a two-region layout and would promise room that does not exist.
 */
import { describe, expect, it } from "vitest";
import { storageFrom, songsFrom, type Song } from "../deviceModel";
import { PHYSICAL_BLOCK_BYTES } from "@/sp1/stemTapeFormat";
import type { StemTapeCapabilities } from "@/sp1/compatibility";

const SONG_BLOCKS = 65536; // 32 MiB per song region
const caps = {
  firmwareId: 0x53544657,
  protoMajor: 1,
  protoMinor: 1,
  formatMajor: 1,
  formatMinor: 2,
  sampleRate: 48000,
  deviceBlocks: SONG_BLOCKS * 2 + 512,
  sectorBytes: 8192,
  alignment: 16,
  flags: 0,
  song: [
    { start: 512, blocks: SONG_BLOCKS },
    { start: 512 + SONG_BLOCKS, blocks: SONG_BLOCKS },
  ],
  index: [
    { start: 0, blocks: 256 },
    { start: 256, blocks: 256 },
  ],
} as unknown as StemTapeCapabilities;

const resident: Song = {
  id: "song-1",
  title: "Resident",
  artist: "",
  durationSeconds: 180,
  sizeBytes: 45056 * PHYSICAL_BLOCK_BYTES,
  isActive: true,
};

describe("device storage capacity", () => {
  it("reports the same capacityBytes before and after a song is stored", () => {
    const empty = storageFrom(caps, []);
    const full = storageFrom(caps, [resident]);

    expect(empty.capacityBytes).toBe(SONG_BLOCKS * PHYSICAL_BLOCK_BYTES);
    expect(full.capacityBytes).toBe(empty.capacityBytes);
    expect(full.maxSongBytes).toBe(empty.maxSongBytes);
  });

  it("never sums the two song regions and never falls back to the device size", () => {
    const empty = storageFrom(caps, []);
    const bothRegions = caps.song[0]!.blocks * 2 * PHYSICAL_BLOCK_BYTES;
    expect(empty.capacityBytes).not.toBe(bothRegions);
    expect(empty.capacityBytes).not.toBe(caps.deviceBlocks * PHYSICAL_BLOCK_BYTES);
  });

  it("moves only used/free with occupancy", () => {
    const empty = storageFrom(caps, []);
    const full = storageFrom(caps, [resident]);
    expect(empty.usedBytes).toBe(0);
    expect(empty.freeBytes).toBe(empty.capacityBytes);
    expect(full.usedBytes).toBe(resident.sizeBytes);
    expect(full.freeBytes).toBe(full.capacityBytes - resident.sizeBytes);
    // A replacement gets the whole region regardless of what is resident.
    expect(full.maxSongBytes).toBe(full.capacityBytes);
  });

  it("an unread device has no capacity at all rather than a guess", () => {
    expect(storageFrom(null, []).capacityBytes).toBe(0);
    expect(songsFrom(null)).toEqual([]);
  });
});
