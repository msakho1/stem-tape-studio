import { createFileRoute, Link } from "@tanstack/react-router";
import { useState } from "react";
import productAsset from "@/assets/sp1-product-3.jpg.asset.json";
import shop1 from "@/assets/sp1-shop-1.png.asset.json";
import shop2 from "@/assets/sp1-shop-2.png.asset.json";
import shop3 from "@/assets/sp1-shop-3.png.asset.json";

const GALLERY = [
  { url: productAsset.url, alt: "Stem Tape for SP-1 — four-fader stem player, three-quarter view" },
  { url: shop1.url, alt: "SP-1 front face — four faders, four track buttons and status LEDs" },
  { url: shop2.url, alt: "SP-1 rear panel — brushed aluminium back with markings and ports" },
  { url: shop3.url, alt: "SP-1 top edge — track buttons, faders, speaker grille and side controls" },
];
import { SupportButton } from "@/components/SupportButton";

export const Route = createFileRoute("/shop")({
  component: ShopPage,
  head: () => ({
    meta: [
      { title: "Stem Tape for SP-1 — Shop" },
      {
        name: "description",
        content:
          "Stem Tape for SP-1: a free four-track tape looper for the browser. Built for performance, designed like hardware, running in real time on your device.",
      },
      { property: "og:title", content: "Stem Tape for SP-1 — Shop" },
      {
        property: "og:description",
        content:
          "A four-track tape looper for the browser. Built for performance. Designed like hardware. Runs in real time.",
      },
      { property: "og:type", content: "product" },
      { name: "twitter:card", content: "summary_large_image" },
    ],
  }),
});

const SECTIONS: { id: string; label: string; body: string }[] = [
  {
    id: "details",
    label: "product details",
    body: "Stem Tape is an unofficial browser digital twin of the SP-1 surface: four stems, four independent tape loops, one shared transport. Every control on the rendered device is playable with pointer, touch, keyboard or MIDI.",
  },
  {
    id: "features",
    label: "features",
    body: "Four-track tape engine with varispeed, reverse and inertia · global and per-stem loops with audible-frame release · four tape heads with audible scrubbing · twelve effects in four banks · automatic tempo, beat-phase and bar detection · Stem Instrument Mode cue markers over MIDI.",
  },
  {
    id: "compatibility",
    label: "compatibility",
    body: "Desktop Chrome, Edge and Safari; iOS and iPadOS Safari for playback. Web MIDI is available in desktop Chrome and Edge. Wired class-compliant USB MIDI on iPhone and iPad runs through the native Stem Tape wrapper.",
  },
  {
    id: "requirements",
    label: "system requirements",
    body: "A current browser with Web Audio and AudioWorklet support, roughly 300 MiB of free memory for four decoded stems, and local audio files. No account, no upload — no audio ever leaves your device.",
  },
];

function ShopPage() {
  const [open, setOpen] = useState<string | null>(null);
  const [license, setLicense] = useState("web application");

  return (
    <div className="min-h-screen">
      {/* ---------- site header ---------- */}
      <header className="border-b border-[var(--bench-line)]">
        <div className="flex items-start justify-between gap-4 px-4 pt-3 md:px-8">
          <p className="font-mono text-[9px] uppercase tracking-[0.24em] text-[var(--ink-faint)] md:text-[10px]">
            unofficial · independent r&amp;d · not affiliated with teenage engineering
          </p>
          <SupportButton />
        </div>
        <div className="flex flex-wrap items-center gap-x-8 gap-y-3 px-4 pb-4 pt-2 md:px-8">
          <Link to="/" className="flex items-center gap-3">
            <svg width="34" height="34" viewBox="0 0 34 34" aria-hidden className="text-[var(--ink)]">
              <path d="M17 5 L30 28 H4 Z" fill="none" stroke="currentColor" strokeWidth="1.2" />
              <path d="M17 13 L23 24 H11 Z" fill="none" stroke="currentColor" strokeWidth="1" opacity="0.6" />
            </svg>
            <div>
              <p className="font-mono text-xl tracking-tight text-[var(--ink)]">Stem Tape</p>
              <p className="font-mono text-[11px] text-[var(--ink-dim)]">shop · sp-1 interface</p>
            </div>
          </Link>
          <nav className="ml-auto flex items-center gap-6" aria-label="Site">
            <Link to="/" className="st-tab">
              instrument
            </Link>
            <Link to="/device" className="st-tab">
              uploader
            </Link>
            <Link to="/about" className="st-tab">
              about
            </Link>
            <span className="st-tab" data-on>
              shop
            </span>
          </nav>
        </div>
      </header>

      {/* breadcrumb */}
      <div className="flex items-center justify-between gap-4 px-4 pb-6 pt-4 md:px-8">
        <p className="font-mono text-[10px] uppercase tracking-[0.22em] text-[var(--ink-dim)]">
          shop <span className="text-[var(--ink-faint)]">/</span> sp-1 interface{" "}
          <span className="text-[var(--ink-faint)]">/</span>{" "}
          <span className="text-[var(--ink)]">stem tape</span>
        </p>
        <Link to="/" className="st-link font-mono text-[10px] uppercase tracking-[0.18em]">
          ← instrument
        </Link>
      </div>

      <main className="mx-auto w-full max-w-[820px] px-4 pb-20 md:px-8">
        {/* product photo */}
        <figure className="relative flex flex-col items-center">
          <img
            src={productAsset.url}
            alt="Stem Tape for SP-1 — four-fader stem player hardware, three-quarter view"
            className="relative z-10 w-full max-w-[560px] select-none"
            draggable={false}
          />
          <figcaption className="mt-4 flex items-center gap-2" aria-hidden>
            {[0, 1, 2, 3, 4].map((i) => (
              <span
                key={i}
                className="h-[5px] w-[5px] rounded-full"
                style={{
                  background: i === 0 ? "var(--ink)" : "var(--bench-line)",
                }}
              />
            ))}
          </figcaption>
        </figure>


        {/* title block */}
        <h1 className="mt-10 font-mono text-[26px] uppercase tracking-[0.06em] text-[var(--ink)] md:text-[32px]">
          Stem Tape for SP-1
        </h1>
        <p className="mt-2 font-mono text-[15px] text-[var(--ink)] underline underline-offset-4">$0.00</p>

        <p className="mt-5 max-w-md font-mono text-[12px] leading-[1.9] text-[var(--ink-dim)]">
          A four-track tape looper for the browser.
          <br />
          Built for performance. Designed like hardware.
          <br />
          Runs in real time.
        </p>

        {/* license */}
        <label className="mt-7 flex items-center justify-between gap-3 border border-[var(--bench-line)] bg-[var(--bench-raised)] px-4 py-4">
          <span className="font-mono text-[11px] uppercase tracking-[0.16em] text-[var(--ink)]">
            license:
          </span>
          <select
            value={license}
            onChange={(e) => setLicense(e.target.value)}
            className="flex-1 cursor-pointer appearance-none bg-transparent font-mono text-[11px] uppercase tracking-[0.16em] text-[var(--ink)] outline-none"
          >
            <option value="web application">web application</option>
            <option value="ios wrapper">ios wrapper</option>
            <option value="firmware source">firmware source</option>
          </select>
          <span aria-hidden className="font-mono text-[11px] text-[var(--ink-dim)]">
            ⌄
          </span>
        </label>

        {/* actions */}
        <button
          type="button"
          className="mt-3 w-full bg-[var(--ink)] px-4 py-4 font-mono text-[11px] uppercase tracking-[0.22em] text-[var(--bench)] transition-opacity hover:opacity-90"
        >
          add to cart
        </button>
        <Link
          to="/"
          className="mt-3 block w-full border border-[var(--bench-line)] px-4 py-4 text-center font-mono text-[11px] uppercase tracking-[0.22em] text-[var(--ink)] transition-colors hover:border-[var(--ink-faint)]"
        >
          ▷ try demo
        </Link>

        {/* accordions */}
        <div className="mt-8 border-t border-[var(--bench-line)]">
          {SECTIONS.map((s) => {
            const on = open === s.id;
            return (
              <div key={s.id} className="border-b border-[var(--bench-line)]">
                <button
                  type="button"
                  aria-expanded={on}
                  onClick={() => setOpen(on ? null : s.id)}
                  className="flex w-full items-center justify-between gap-4 py-4 font-mono text-[11px] uppercase tracking-[0.18em] text-[var(--ink)]"
                >
                  {s.label}
                  <span aria-hidden className="text-[var(--ink-dim)]">
                    {on ? "⌄" : "›"}
                  </span>
                </button>
                {on && (
                  <p className="-mt-1 pb-4 font-mono text-[11px] leading-relaxed text-[var(--ink-dim)]">
                    {s.body}
                  </p>
                )}
              </div>
            );
          })}
        </div>

        <p className="mt-10 text-center font-mono text-[9px] uppercase tracking-[0.2em] text-[var(--ink-faint)]">
          unofficial · independent r&amp;d · not affiliated with teenage engineering
        </p>
      </main>
    </div>
  );
}
