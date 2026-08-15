import { createFileRoute, Link } from "@tanstack/react-router";
import { SupportButton } from "@/components/SupportButton";

export const Route = createFileRoute("/about")({
  component: AboutPage,
  head: () => ({
    meta: [
      { title: "About — Stem Tape" },
      {
        name: "description",
        content:
          "Stem Tape is an experimental music instrument that turns a finished song into four pieces of playable tape. Built by Mounir Sakho.",
      },
      { property: "og:title", content: "About — Stem Tape" },
      {
        property: "og:description",
        content:
          "A song is no longer a file. It’s an instrument. Stem Tape turns stems into playable tape.",
      },
      { property: "og:type", content: "website" },
      { name: "twitter:card", content: "summary_large_image" },
    ],
  }),
});

function AboutPage() {
  return (
    <div className="min-h-screen">
      {/* ---------- site header ---------- */}
      <header className="border-b border-[var(--bench-line)]">
        <div className="flex items-start justify-between gap-4 px-4 pt-3 md:px-8">
          <p className="font-mono text-[9px] uppercase tracking-[0.24em] text-[var(--ink-faint)] md:text-[10px]">
            not affiliated with teenage engineering
          </p>
          <div className="flex shrink-0 items-center gap-4">
            <nav className="hidden items-center gap-6 lg:flex" aria-label="Site">
              <Link to="/" className="st-tab">
                instrument
              </Link>
              <Link to="/shop" className="st-tab">
                shop
              </Link>
              <span className="st-tab" data-on>
                about
              </span>
            </nav>
            <SupportButton />
          </div>
        </div>
        <div className="flex flex-wrap items-center gap-x-8 gap-y-3 px-4 pb-4 pt-2 md:px-8">
          <Link to="/" className="flex items-center gap-3">
            <svg width="34" height="34" viewBox="0 0 34 34" aria-hidden className="text-[var(--ink)]">
              <path d="M17 5 L30 28 H4 Z" fill="none" stroke="currentColor" strokeWidth="1.2" />
              <path d="M17 13 L23 24 H11 Z" fill="none" stroke="currentColor" strokeWidth="1" opacity="0.6" />
            </svg>
            <div>
              <p className="font-mono text-xl tracking-tight text-[var(--ink)]">Stem Tape</p>
              <p className="font-mono text-[11px] text-[var(--ink-dim)]">about · project · creator</p>
            </div>
          </Link>
          <nav className="ml-auto flex items-center gap-6 lg:hidden" aria-label="Site">
            <Link to="/" className="st-tab">
              instrument
            </Link>
            <Link to="/shop" className="st-tab">
              shop
            </Link>
            <span className="st-tab" data-on>
              about
            </span>
          </nav>
        </div>
      </header>

      <main className="mx-auto w-full max-w-[760px] px-4 pb-20 pt-8 md:px-8">
        <section className="st-section">
          <p className="st-section__title">about stem tape</p>

          <p className="font-mono text-[22px] leading-[1.35] tracking-tight text-[var(--ink)] md:text-[28px]">
            A song is no longer a file. It’s an instrument.
          </p>

          <div className="mt-6 space-y-4 font-mono text-[12px] leading-[1.85] text-[var(--ink-dim)]">
            <p>
              STEM TAPE is an experimental music instrument built around a simple idea: what if you could reach inside a finished song and play it?
            </p>
            <p>
              Vocals. Drums. Bass. Music.
            </p>
            <p>
              Instead of treating stems as files to arrange on a screen, STEM TAPE turns them into four pieces of playable tape — something you can mute, isolate, loop, manipulate, perform and recombine in real time.
            </p>
            <p>
              The project began as an exploration of the Teenage Engineering SP-1 Stem Player: a beautifully simple piece of hardware with possibilities far beyond its original software.
            </p>
            <p>
              STEM TAPE pushes that idea further.
            </p>
            <p>
              It combines stem separation with the physical language of tape manipulation, live performance and sampling — including independent stem control, looping, effects, transport manipulation, MIDI control and experimental modes like HEADS, where different moments of a recording can exist and play simultaneously.
            </p>
            <p>
              The browser instrument makes the system accessible to anyone. The physical edition brings STEM TAPE back to the hardware that inspired it, with the software preloaded and ready to play.
            </p>
          </div>

          <div className="mt-8 border-t border-[var(--bench-line)] pt-5">
            <ul className="space-y-2 font-mono text-[11px] uppercase tracking-[0.16em] text-[var(--ink)]">
              <li>No timeline.</li>
              <li>No arrangement view.</li>
              <li>No DAW required.</li>
            </ul>
            <p className="mt-5 font-mono text-[13px] uppercase tracking-[0.12em] text-[var(--signal)]">
              Play the recording.
            </p>
          </div>
        </section>

        <section className="st-section mt-5">
          <p className="st-section__title">about mounir sakho</p>

          <p className="font-mono text-[18px] leading-[1.35] tracking-tight text-[var(--ink)] md:text-[22px]">
            Designer. Creative director. Builder.
          </p>

          <div className="mt-5 space-y-4 font-mono text-[12px] leading-[1.85] text-[var(--ink-dim)]">
            <p>
              Mounir Sakho is a New York–based designer and the creator of STEM TAPE.
            </p>
            <p>
              His work sits between design, music, technology and physical objects — often beginning with a question about how something familiar could behave differently.
            </p>
            <p>
              STEM TAPE began with one of those questions.
            </p>
            <p>
              After discovering Teenage Engineering’s SP-1 Stem Player, Mounir became interested not simply in what the device was designed to do, but in what its physical interface could become.
            </p>
            <p>
              That exploration evolved into STEM TAPE: first as an interactive browser instrument, then as a system designed to live on the original hardware itself.
            </p>
            <p>
              Mounir is also the founder and creative director of LUMÈRE, an independent fashion label built around the idea of clothing as a system of presence.
            </p>
            <p>
              Across mediums, the approach remains the same:
            </p>
          </div>

          <div className="mt-6 border-t border-[var(--bench-line)] pt-5">
            <ul className="space-y-2 font-mono text-[11px] uppercase tracking-[0.16em] text-[var(--ink)]">
              <li>Take an object seriously.</li>
              <li>Question its assumptions.</li>
              <li>Build the thing you wish existed.</li>
            </ul>
          </div>
        </section>

        <p className="mt-10 text-center font-mono text-[9px] uppercase tracking-[0.2em] text-[var(--ink-faint)]">
          not affiliated with teenage engineering
        </p>
      </main>
    </div>
  );
}
