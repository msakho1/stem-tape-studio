"""
Real-browser pointer proof for PLAY first-claim ownership (449 / 450 / 700 ms).

Fake timers prove the arbiter's logic; this drives real PointerEvents on the
rendered SP-1 hit zones. All scheduling happens INSIDE the page with a
performance.now() spin so the measured PLAY->Track and overlap deltas are the
real ones, not Playwright IPC jitter. Commands are read from window.__stemTape.
"""

import asyncio, json, sys
from playwright.async_api import async_playwright

URL = "http://localhost:8080/"

RUN = """async ([trackDelay, overlap]) => {
  const q = (c) => document.querySelector(`[data-control="${c}"]`);
  const fire = (c, type, id) => {
    const el = q(c), r = el.getBoundingClientRect();
    el.dispatchEvent(new PointerEvent(type, {
      pointerId: id, pointerType: 'touch', isPrimary: id === 1,
      bubbles: true, cancelable: true,
      clientX: r.x + r.width / 2, clientY: r.y + r.height / 2,
    }));
  };
  const waitUntil = async (deadline) => {
    while (performance.now() < deadline - 2) await new Promise(r => setTimeout(r, 1));
    while (performance.now() < deadline) { /* spin the final 2 ms */ }
  };
  window.__stemTape.commands.length = 0;

  const t0 = performance.now();
  fire('play', 'pointerdown', 1);
  await waitUntil(t0 + trackDelay);
  const tTrack = performance.now();
  fire('track-button-1', 'pointerdown', 2);
  await waitUntil(tTrack + overlap);
  const tUp = performance.now();
  fire('track-button-1', 'pointerup', 2);
  fire('play', 'pointerup', 1);
  await new Promise(r => setTimeout(r, 600));
  return {
    playToTrackMs: +(tTrack - t0).toFixed(1),
    overlapMs: +(tUp - tTrack).toFixed(1),
    commands: window.__stemTape.commands.map(c => c.type),
  };
}"""


async def main():
    out = {}
    async with async_playwright() as p:
        b = await p.chromium.launch(headless=True)
        ctx = await b.new_context(viewport={"width": 1280, "height": 1800}, has_touch=True)
        page = await ctx.new_page()
        await page.goto(URL, wait_until="domcontentloaded")
        await page.wait_for_selector('[data-control="play"]')
        await page.wait_for_function("() => !!window.__stemTape")

        cases = {
            "claim_449_release_250": (449, 250),   # inside claim window, short overlap -> solo
            "late_450_release_250": (450, 250),    # boundary: PLAY owned by hold -> no chord
            "claim_200_hold_700": (200, 720),      # overlap reaches 700 ms -> link
        }
        for name, (d, o) in cases.items():
            out[name] = await page.evaluate(RUN, [d, o])
            await asyncio.sleep(0.4)
        await b.close()

    print(json.dumps(out, indent=2))
    fails = []

    a = out["claim_449_release_250"]
    if a["playToTrackMs"] >= 450:
        fails.append("case 1 missed the claim window (measured %.1f ms)" % a["playToTrackMs"])
    if "stem.solo" not in a["commands"]:
        fails.append("case 1: no stem.solo")
    leak = [t for t in a["commands"] if t.startswith("transport.") or t.startswith("loop.global") or t.startswith("track.")]
    if leak:
        fails.append(f"case 1 leaked base commands: {leak}")

    b2 = out["late_450_release_250"]
    if b2["playToTrackMs"] < 450:
        fails.append("case 2 landed inside the claim window (%.1f ms)" % b2["playToTrackMs"])
    if "stem.solo" in b2["commands"] or "stem.link" in b2["commands"]:
        fails.append(f"case 2 leaked a chord: {b2['commands']}")

    c = out["claim_200_hold_700"]
    if "stem.link" not in c["commands"]:
        fails.append(f"case 3: no stem.link ({c['commands']})")
    if "stem.solo" in c["commands"]:
        fails.append("case 3 emitted solo as well as link")

    for f in fails:
        print("FAIL:", f, file=sys.stderr)
    print("PASS" if not fails else "FAIL")
    sys.exit(0 if not fails else 1)


asyncio.run(main())
