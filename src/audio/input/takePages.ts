/**
 * Project-level take page budget (binding correction M1).
 *
 * ONE budget for the whole project, sized from the layers that can be audible
 * at the same time plus the required read-ahead — never a fixed per-take cache.
 * If activating another layer would exceed the budget the activation is
 * REJECTED with a reason; a silent underrun is not an acceptable outcome.
 */

export interface PageKey {
  trackId: number;
  layerId: string;
  pageIndex: number;
}

export interface BudgetSnapshot {
  budgetBytes: number;
  residentBytes: number;
  residentPages: number;
  activeLayers: number;
  pageFrames: number;
  channels: number;
  readAheadPages: number;
  misses: number;
  underruns: number;
  evictions: number;
  rejections: string[];
}

export class TakePageBudgetManager {
  /** Insertion-ordered = LRU order; refreshed on touch. */
  private resident = new Map<string, number>();
  private layers = new Set<string>();
  misses = 0;
  underruns = 0;
  evictions = 0;
  readonly rejections: string[] = [];

  constructor(
    public budgetBytes: number,
    public pageFrames = 24000,
    public channels = 1,
    public readAheadPages = 2,
  ) {}

  private bytesPerPage(): number {
    return this.pageFrames * this.channels * 4;
  }

  static key(k: PageKey): string {
    return `${k.trackId}:${k.layerId}:${k.pageIndex}`;
  }

  /** Bytes a layer needs resident at minimum to play without underrunning. */
  layerFootprint(): number {
    return (this.readAheadPages + 2) * this.bytesPerPage();
  }

  residentBytes(): number {
    return this.resident.size * this.bytesPerPage();
  }

  /** Gate BEFORE a layer is activated (M1): reject rather than underrun. */
  canActivate(extraLayers = 1): { ok: boolean; detail: string } {
    const projected = (this.layers.size + extraLayers) * this.layerFootprint();
    if (projected <= this.budgetBytes)
      return {
        ok: true,
        detail: `${(projected / 1048576).toFixed(1)} MiB of ${(this.budgetBytes / 1048576).toFixed(1)} MiB take-page budget with ${this.layers.size + extraLayers} audible layers`,
      };
    const rejection = `rejected: ${this.layers.size + extraLayers} audible take layers need ${(projected / 1048576).toFixed(1)} MiB of pages, budget is ${(this.budgetBytes / 1048576).toFixed(1)} MiB — freeze or bounce a layer first`;
    return { ok: false, detail: rejection };
  }

  activate(layerId: string): { ok: boolean; detail: string } {
    if (this.layers.has(layerId)) return { ok: true, detail: `${layerId} already active` };
    const verdict = this.canActivate(1);
    if (!verdict.ok) {
      this.rejections.unshift(verdict.detail);
      this.rejections.length = Math.min(this.rejections.length, 10);
      return verdict;
    }
    this.layers.add(layerId);
    return verdict;
  }

  deactivate(layerId: string) {
    this.layers.delete(layerId);
    for (const k of [...this.resident.keys()]) if (k.includes(`:${layerId}:`)) this.resident.delete(k);
  }

  /** Record a delivered page; returns the pages that must be evicted. */
  admit(k: PageKey): PageKey[] {
    const key = TakePageBudgetManager.key(k);
    this.resident.delete(key);
    this.resident.set(key, Date.now());
    const maxPages = Math.max(1, Math.floor(this.budgetBytes / this.bytesPerPage()));
    const evict: PageKey[] = [];
    while (this.resident.size > maxPages) {
      const oldest = this.resident.keys().next().value as string;
      this.resident.delete(oldest);
      const [t, layerId, p] = oldest.split(":");
      evict.push({ trackId: Number(t), layerId: layerId!, pageIndex: Number(p) });
      this.evictions++;
    }
    return evict;
  }

  touch(k: PageKey) {
    const key = TakePageBudgetManager.key(k);
    if (this.resident.has(key)) {
      this.resident.delete(key);
      this.resident.set(key, Date.now());
    }
  }

  has(k: PageKey): boolean {
    return this.resident.has(TakePageBudgetManager.key(k));
  }

  noteMiss() {
    this.misses++;
  }

  noteUnderrun(n = 1) {
    this.underruns += n;
  }

  snapshot(): BudgetSnapshot {
    return {
      budgetBytes: this.budgetBytes,
      residentBytes: this.residentBytes(),
      residentPages: this.resident.size,
      activeLayers: this.layers.size,
      pageFrames: this.pageFrames,
      channels: this.channels,
      readAheadPages: this.readAheadPages,
      misses: this.misses,
      underruns: this.underruns,
      evictions: this.evictions,
      rejections: [...this.rejections],
    };
  }
}
