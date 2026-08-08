# Stem Tape — Phase 1: Feasibility Investigation (report only)

Scope confirmed: build the in-app technical assessment only. Simulator, dashboard trackers and importer prototype come later. Data persists in Lovable Cloud. All claims are sourced and labelled.

## What the research already establishes

Fetched from the repos/wikis you listed. Every claim below is quoted or derived from those pages, and the app will carry the same evidence labels.

- **DOCUMENTED — SoC:** nRF52840, Cortex-M4 @ 64 MHz, 1 MB flash, **256 kB RAM** (SP-1-dev wiki, Hardware overview).
- **DOCUMENTED — Storage:** Toshiba/Kioxia THGBMNG5D1LBAIL 4 GB eMMC on a **1-bit** bus (CLK/CMD/DAT0/RST_n). Tape Looper drives data via SPIM3 DMA at 32 MHz.
- **DOCUMENTED — No filesystem:** 8192-byte sectors; sector 0 is album metadata; audio starts at 0x2000. Album header magic `ALBUM_PRESENT`, 4-byte length, 1-byte song count, 64-byte title; per-song entries carry start sector, length, 64-byte artist, 64-byte title.
- **DOCUMENTED — Audio format:** 24-bit / 48 kHz / **8 channels (4 stereo stems)**, 340 frames per sector, 24-byte frames, plus per-sector timing/tempo/LED bytes, with non-sequential 0,2,1,3 block interleave and an unconventional per-stem 6-byte L/R byte order.
- **DOCUMENTED — Stock USB:** wired for **charging + CDC-ACM serial only**. No USB Audio, no MSC, no USB MIDI on stock. Stem loading today goes over **Web Serial** via solderless.engineering (Chrome/Edge only).
- **DOCUMENTED — Tape Looper:** Zephyr v4.3.1 / SDK 0.17.4, MIT licensed. Files: `firmware/src/main.c`, `firmware/src/sp1_emmc.c`, `boards/teenageengineering/stem_player/stem_player.dts`, `zephyr-patches/uac2-windows-fs-feedback.patch`. Threads: `audio_thread` (per 256-frame I2S block, mixes 4 tracks + USB monitor), `streamer_thread` (sole flash owner), `midi_thread`.
- **DOCUMENTED — Tape Looper USB audio:** UAC2, **Full-Speed, 48 kHz, 16-bit, STEREO (2 ch), host→device only**. There is no capture direction and no multichannel path in any open repo.
- **DOCUMENTED — Bootloader constraints:** app at 0x20000, max ~0xDF000, 5 s watchdog, no reset pin (firmware must reach SYSTEM_OFF), RESETREAS must be cleared.
- **UNVERIFIED:** stock stem-upload byte protocol (wiki stub, Solderless tool closed-source); Advanced Mode semantics; exact MIDI clock / PO sync framing; user reports of slow upload (not found in these sources).

### The two findings that reshape the concept

1. **The fast importer is not a stock-firmware feature — it requires custom firmware.** Stock USB is CDC-ACM only. A 4-channel audio import path only exists inside a Stem Tape firmware that extends Tape Looper's UAC2 node from 2 to 4 channels. The report will state this plainly rather than implying a browser can stream into a stock SP-1.
2. **Bandwidth is probably not the binding constraint; RAM and eMMC are.** 4 mono ch @ 48 kHz/16-bit = 384 kB/s ≈ 3.07 Mbit/s, inside USB Full-Speed isochronous headroom (~8.2 Mbit/s). But the native store is 24-bit 8-channel ≈ 1.15 MB/s sustained onto a 1-bit eMMC, against 256 kB total RAM for all ring buffers. The report treats **sustained eMMC write + RAM budget** as risk #1, and stereo-in (8 ch) as a distinctly later experiment. Note also: a real-time streaming import of a 5-minute song takes ~5 minutes by definition — the stated target is met by the transport being real-time, not by being fast.

## Report structure to build

Routes under `/` (overview) with sections as separate routes so each is linkable and gets its own head metadata:

- `/` — project overview, thesis, non-goals, legal/licensing stance, evidence-label legend.
- `/capability-matrix` — rows = capabilities from your brief (stock set + Tape Looper set + Stem Tape additions), columns = Stock SP-1 / Tape Looper 2.0 / Proposed Stem Tape, each cell tagged Documented / Inferred / Unverified / Needs hardware, with a source link per row.
- `/architecture` — system diagram: companion importer (Web Serial + WebAudio) → USB transport (UAC2 4ch vs CDC bulk, both shown as candidate paths) → firmware audio engine (audio_thread / streamer_thread / midi_thread) → 4-track mixer → eMMC sector writer → album index → global transport + per-track state → MIDI/PO sync → codec output. Rendered as inline SVG/CSS, no diagram library.
- `/firmware-plan` — subsystem table keyed to real Tape Looper paths (`main.c` audio_thread, `sp1_emmc.c` writer, `stem_player.dts` uac2 node, the Windows feedback patch), each with change description, difficulty, owner (Claude Code / embedded dev / hardware test).
- `/experiments` — the nine critical hardware experiments, each with hypothesis, method, pass criteria, blocking dependencies, and status (all `not started`).
- `/risks` — register with likelihood/impact/mitigation, seeded with the twelve risks in your brief plus the RAM/eMMC finding above.
- `/roadmap` — eleven phases from research to open-source release, with the gating experiment for each.
- `/sources` — every repo, wiki page and tool, with license (both key repos are MIT) and attribution requirements.

Each section also states **ownership**: firmware vs companion app vs Claude Code vs physical SP-1 vs "simulated in this Lovable project". A persistent banner marks the app as an R&D planning tool with no hardware connection implemented.

## Technical details

- Stack as-is: TanStack Start, file routes, Tailwind v4 tokens in `src/styles.css`.
- **Lovable Cloud** enabled. Tables: `capabilities`, `experiments`, `risks`, `firmware_subsystems`, `roadmap_phases`, `sources`, `open_questions`. Each row carries `evidence_label` (documented/inferred/unverified/needs_hardware) and `source_url`. Migration includes schema, GRANTs, RLS, and literal INSERTs seeding the full researched content so the report renders complete on first load.
- Phase 1 is read-mostly: public anon SELECT so the report is shareable; authenticated write policies are created now for the later dashboard, no auth UI yet.
- Design: restrained technical instrument language — near-monochrome neutrals, one signal accent, monospace numerics and labels, hairline rules, dense tabular layout. No TE logos, product renders or protected graphics.
- Wording rules enforced throughout: no claim of working hardware transfer; every capability cell carries its evidence label.

## Not in this phase

Four-track simulator, dashboard trackers (build history, device inventory, community feedback), and the companion importer prototype — planned next once the assessment is agreed.
