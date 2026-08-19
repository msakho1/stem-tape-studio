import { createFileRoute, Link } from "@tanstack/react-router";
import { useState } from "react";
import hero from "@/assets/sp1-hero-grey.png.asset.json";
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

const SECTIONS: { id: string; label: string; body: string[] }[] = [
  {
    id: "details",
    label: "product description",
    body: [
      "Teenage Engineering SP-1, fully loaded with Stem Tape.",
      "A ready-to-play SP-1 with the latest Stem Tape firmware installed, configured, and individually tested for full functionality before shipping.",
      "No setup. Take it out of the box, load your music, and start playing.",
      "Stem Tape transforms the SP-1 into a four-track performance instrument built around stems — giving you independent, tactile control over the parts of a song and a growing set of tools for manipulating music in real time.",
      "Every unit is a genuine Teenage Engineering SP-1 and is tested with Stem Tape before it leaves us.",
      "All proceeds from these units go directly toward the continued development of Stem Tape and keeping the platform available to everyone.",
    ],
  },
  {
    id: "features",
    label: "features",
    body: [
      "4-TRACK STEM PLAYBACK — Load a song as four synchronized stems and control each part independently from the SP-1's four physical channels.",
      "PHYSICAL STEM CONTROL — Four faders give you immediate hands-on control over the mix. Bring vocals, drums, bass, instruments, or any other stem in and out while the song plays.",
      "INDEPENDENT TRACK CONTROL — Interact with individual stems without breaking synchronization between the four tracks.",
      "LOOPING — Create and manipulate loops directly from the hardware for live performance, practice, remixing, and experimentation.",
      "VARISPEED PLAYBACK — Manipulate playback speed from the SP-1 for tape-inspired pitch and speed changes.",
      "STEM TAPE PERFORMANCE CONTROLS — Stem Tape combines the SP-1's physical interface with an expanded performance-oriented control system designed specifically around manipulating separated music.",
      "SONG STORAGE — Store songs on the SP-1 and recall them without needing a computer during playback.",
      "STEM TAPE COMPANION APP — Prepare and transfer your four-stem songs to the SP-1 through the Stem Tape companion experience.",
      "READY OUT OF THE BOX — Stem Tape comes installed and configured. Each device is tested before shipping so you can start using it immediately.",
    ],
  },
  {
    id: "support",
    label: "built to support the project",
    body: [
      "Stem Tape is an independent project exploring what the SP-1 can become when its hardware is opened up to a different way of interacting with music.",
      "The Stem Tape platform and firmware are being developed for the community. Purchasing a preloaded SP-1 is a way to get a ready-to-use physical Stem Tape instrument while directly funding continued development.",
      "Buy the hardware if you want the ready-to-play experience. The project itself remains for everyone.",
    ],
  },
];

function ShopPage() {
  const [open, setOpen] = useState<string | null>(null);

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
            src={(GALLERY[shot] ?? GALLERY[0]!).url}
            alt={(GALLERY[shot] ?? GALLERY[0]!).alt}
            className="relative z-10 w-full max-w-[560px] select-none"
            draggable={false}
          />
          <figcaption className="mt-4 flex items-center gap-2">
            {GALLERY.map((g, i) => (
              <button
                key={g.url}
                type="button"
                aria-label={`View photo ${i + 1}`}
                aria-current={i === shot}
                onClick={() => setShot(i)}
                className="h-[9px] w-[9px] rounded-full p-0"
                style={{
                  background: i === shot ? "var(--ink)" : "var(--bench-line)",
                }}
              />
            ))}
          </figcaption>
          <div className="mt-4 flex flex-wrap justify-center gap-2">
            {GALLERY.map((g, i) => (
              <button
                key={g.url}
                type="button"
                onClick={() => setShot(i)}
                className="border p-1 transition-colors"
                style={{
                  borderColor: i === shot ? "var(--ink)" : "var(--bench-line)",
                }}
              >
                <img src={g.url} alt={g.alt} className="h-14 w-14 object-contain" draggable={false} />
              </button>
            ))}
          </div>
        </figure>


        {/* title block */}
        <h1 className="mt-10 font-mono text-[26px] uppercase tracking-[0.06em] text-[var(--ink)] md:text-[32px]">
          Stem Tape for SP-1
        </h1>
        <p className="mt-2 font-mono text-[15px] text-[var(--ink)] underline underline-offset-4">$175</p>

        <p className="mt-5 max-w-md font-mono text-[12px] leading-[1.9] text-[var(--ink-dim)]">
          Teenage Engineering SP-1, fully loaded with Stem Tape.
          <br />
          Installed, configured and individually tested.
          <br />
          No setup — take it out of the box and play.
        </p>

        {/* actions */}
        <button
          type="button"
          className="mt-7 w-full bg-[var(--ink)] px-4 py-4 font-mono text-[11px] uppercase tracking-[0.22em] text-[var(--bench)] transition-opacity hover:opacity-90"
        >
          support this project
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
                  <div className="-mt-1 grid gap-3 pb-4">
                    {s.body.map((para) => (
                      <p
                        key={para}
                        className="font-mono text-[11px] leading-relaxed text-[var(--ink-dim)]"
                      >
                        {para}
                      </p>
                    ))}
                  </div>
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
