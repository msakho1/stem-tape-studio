/**
 * Guide coverage — by FEATURE ID, not by card count.
 *
 * The Guide is 21 animated lessons (the twenty-first is Stem Instrument Mode)
 * + 4 FX bank accordions + one keyboard table + compact reference sections +
 * one hardware-only section. All 84 documented features must appear on one of
 * those surfaces; nothing requires one card per feature.
 */

import { describe, expect, it } from "vitest";
import {
  FEATURE_IDS,
  FX_BANK_CARDS,
  HARDWARE_SECTION,
  LESSONS,
  REFERENCE_SECTIONS,
  guideCoverage,
  keyboardTable,
} from "@/device/guideContent";

describe("guide structure", () => {
  it("has exactly 21 animated performance lessons", () => {
    expect(LESSONS).toHaveLength(21);
  });

  it("has four FX bank accordions carrying all twelve algorithms", () => {
    expect(FX_BANK_CARDS).toHaveLength(4);
    const algos = FX_BANK_CARDS.flatMap((b) => b.algorithms.map((a) => a.id));
    expect(new Set(algos).size).toBe(12);
  });

  it("has one complete keyboard table with every group populated", () => {
    for (const { group, rows } of keyboardTable()) {
      expect(rows.length, `keyboard group ${group.feature} is empty`).toBeGreaterThan(0);
    }
  });

  it("has compact reference sections and one hardware-only section", () => {
    expect(REFERENCE_SECTIONS.map((s) => s.id)).toEqual(["projects", "loading", "session", "system"]);
    expect(HARDWARE_SECTION.entries.length).toBeGreaterThan(0);
  });
});

describe("feature coverage", () => {
  it("documents exactly 84 unique features", () => {
    expect(new Set(FEATURE_IDS).size).toBe(84);
    expect(FEATURE_IDS).toHaveLength(84);
  });

  it("covers all 84 features somewhere in the guide", () => {
    const covered = guideCoverage();
    const missing = FEATURE_IDS.filter((id) => !covered.has(id));
    expect(missing).toEqual([]);
  });

  it("claims no feature that is not in the catalogue", () => {
    const known = new Set(FEATURE_IDS);
    const unknown = [...guideCoverage()].filter((id) => !known.has(id));
    expect(unknown).toEqual([]);
  });
});
