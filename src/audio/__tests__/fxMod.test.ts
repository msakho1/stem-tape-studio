import { describe, expect, it } from "vitest";

import {
  BANKS,
  LEGACY_FAMILY_TO_BANK,
  algorithmDef,
  bankOfButton,
  initialStemFx,
  migrateLegacyStemFx,
  type AlgorithmId,
} from "@/machine/fx12";
import { FX_FAMILIES, BANK_FAMILY } from "@/machine/stemPerformance";

const ALL_IDS = BANKS.flatMap((b) => b.algorithms.map((a) => a.id)) as AlgorithmId[];

describe("MOD bank replaces Beat Repeat and Pump", () => {
  it("Beat Repeat and Pump no longer exist anywhere in the registry", () => {
    expect(ALL_IDS).not.toContain("beatRepeat" as AlgorithmId);
    expect(ALL_IDS).not.toContain("pump" as AlgorithmId);
    expect(ALL_IDS.length).toBe(12);
    expect(new Set(ALL_IDS).size).toBe(12);
  });

  it("physical Button 4 selects MOD: Reel Flange · Formant Shift · Rhythmic Gate", () => {
    const bank = bankOfButton(3);
    expect(BANKS[bank]!.id).toBe("mod");
    expect(BANKS[bank]!.label).toBe("MOD");
    expect(BANKS[bank]!.algorithms.map((a) => a.id)).toEqual(["reelFlange", "formantShift", "gate"]);
    expect(algorithmDef(bank, 1).label).toBe("Formant Shift");
  });

  it("no MOD algorithm is a legacy Phase 5C processor", () => {
    const bank = bankOfButton(3);
    for (const a of BANKS[bank]!.algorithms) expect(a.legacy).toBe(false);
    // ...and the bank therefore maps to NO legacy family.
    expect(BANK_FAMILY[bank]).toBeNull();
    expect(FX_FAMILIES).toEqual(["filter", "echo", "reverb"]);
  });

  it("a saved v3 Beat Repeat latch migrates to MOD / Rhythmic Gate, not to a missing processor", () => {
    expect(LEGACY_FAMILY_TO_BANK["beatRepeat"]).toEqual({ bank: 1, algorithm: 2 });
    const migrated = migrateLegacyStemFx({ beatRepeat: { latched: true, variation: 3 } });
    const bank = migrated.banks[1]!;
    expect(bank.latched).toBe(true);
    expect(bank.selectedAlgorithm).toBe(2);
    expect(algorithmDef(1, bank.selectedAlgorithm).id).toBe("gate");
    // variation 3 of 4 → macro (3-1)/3
    expect(bank.algorithms[2]!.macroAmount).toBeCloseTo(2 / 3, 12);
  });

  it("fresh projects open the MOD bank on Reel Flange with its default macro", () => {
    const fx = initialStemFx();
    const bank = fx.banks[1]!;
    expect(bank.selectedAlgorithm).toBe(0);
    expect(bank.latched).toBe(false);
    expect(bank.algorithms[0]!.macroAmount).toBeCloseTo(algorithmDef(1, 0).defaultMacro, 12);
  });
});
