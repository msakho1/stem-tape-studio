import { createHash } from "node:crypto";
import { it } from "vitest";
import { decodeWavFixture, offlineStub } from "@/sp1/__tests__/fixtureWav";
import { prepareCanonicalSong } from "@/sp1/song";
import { encodeSong } from "@/sp1/sector";
import { sectorToGroups, readGroupHeader } from "@/sp1/sector";

it("hash", async () => {
  const NAMES = ["vocal", "drums", "bass", "instrument"] as const;
  const inputs = NAMES.map((name) => ({ name, filename: `${name}.wav`, buffer: decodeWavFixture(name) }));
  const song = await prepareCanonicalSong(inputs, { metadata: { title: "Fixture Song", artist: "Stem Tape Tests", bpm: 96, downbeatSeconds: 0.25 }, make: offlineStub });
  const sectors = encodeSong(song);
  const total = sectors.reduce((n, s) => n + s.length, 0);
  const buf = new Uint8Array(total);
  let o = 0; for (const s of sectors) { buf.set(s, o); o += s.length; }
  const h = createHash("sha256").update(buf).digest("hex");
  console.log("BYTES", total, "SECTORS", sectors.length);
  console.log("SHA256", h);
  console.log("SECTOR10", sectorToGroups(sectors[10]!).map((g) => { const x = readGroupHeader(g); return `(${x.stemIndex},${x.groupIndex})`; }).join(" "));
});
