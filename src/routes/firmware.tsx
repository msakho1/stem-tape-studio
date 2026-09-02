import { createFileRoute, Link } from "@tanstack/react-router";
import stemTapeLogo from "@/assets/stemtape-logo.png.asset.json";
import { SupportButton } from "@/components/SupportButton";

export const Route = createFileRoute("/firmware")({
  component: FirmwarePage,
  head: () => ({
    meta: [
      { title: "Firmware — Stem Tape" },
      {
        name: "description",
        content:
          "Load STEM TAPE directly to your SP-1. Firmware updates and installation tools coming soon.",
      },
      { property: "og:title", content: "Firmware — Stem Tape" },
      {
        property: "og:description",
        content: "Load STEM TAPE directly to your SP-1. Coming soon.",
      },
      { property: "og:type", content: "website" },
      { name: "twitter:card", content: "summary_large_image" },
    ],
  }),
});

function FirmwarePage() {
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
              <Link to="/device" className="st-tab">
                uploader
              </Link>
              <Link to="/about" className="st-tab">
                about
              </Link>
              <span className="st-tab" data-on>
                firmware
              </span>
            </nav>
            <SupportButton />
          </div>
        </div>
        <div className="flex flex-wrap items-center gap-x-8 gap-y-3 px-4 pb-3 pt-2 md:px-8 md:pb-4">
          <Link to="/" className="flex items-center gap-3">
            <img src={stemTapeLogo.url} alt="Stem Tape logo" width={120} height={120} className="h-16 w-16 object-contain md:h-[120px] md:w-[120px]" />
            <div>
              <p className="font-mono text-xl tracking-tight text-[var(--ink)]">Stem Tape</p>
              <p className="font-mono text-[11px] text-[var(--ink-dim)]">firmware · sp-1</p>
            </div>
          </Link>
          <nav className="ml-auto flex items-center gap-6 lg:hidden" aria-label="Site">
            <Link to="/" className="st-tab">
              instrument
            </Link>
            <Link to="/shop" className="st-tab">
              shop
            </Link>
            <Link to="/device" className="st-tab">
              uploader
            </Link>
            <Link to="/about" className="st-tab">
              about
            </Link>
            <span className="st-tab" data-on>
              firmware
            </span>
          </nav>
        </div>
      </header>

      <main className="mx-auto flex w-full max-w-[760px] flex-col items-start px-4 pb-20 pt-10 md:px-8">
        <section className="st-section w-full">
          <p className="st-section__title">sp-1 firmware</p>
          <p className="font-mono text-[22px] leading-[1.35] tracking-tight text-[var(--ink)] md:text-[28px]">
            load STEM TAPE directly to your SP-1.
          </p>
          <p className="mt-5 font-mono text-[13px] uppercase tracking-[0.22em] text-[var(--signal)]">
            COMING SOON
          </p>
          <p className="mt-6 max-w-md font-mono text-[12px] leading-[1.85] text-[var(--ink-dim)]">
            A browser-based firmware installer for the Teenage Engineering SP-1. Connect your device, flash the latest Stem Tape firmware, and keep your hardware up to date without leaving the site.
          </p>
        </section>

        <p className="mt-10 text-center font-mono text-[9px] uppercase tracking-[0.2em] text-[var(--ink-faint)]">
          not affiliated with teenage engineering
        </p>
      </main>
    </div>
  );
}
