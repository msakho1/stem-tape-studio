"""
Real-browser pointer proof for PLAY first-claim ownership.

Fake timers prove the arbiter's logic; this drives actual PointerEvents on the
rendered SP-1 hit zones with real wall-clock spacing around the 449 / 450 / 700
ms boundaries and reads the emitted command stream from window.__stemTape.
"""

import asyncio, json, sys
from playwright.async_api import async_playwright

URL = "http://localhost:8080/"

DOWN = """([sel, id]) => {
  const el = document.querySelector(`[data-control="${sel}"]`);
  const r = el.getBoundingClientRect();
  const o = {pointerId:id, pointerType:'touch', isPrimary:id===1, bubbles:true, cancelable:true,
             clientX:r.x+r.width/2, clientY:r.y+r.height/2};
  el.dispatchEvent(new PointerEvent('pointerdown', o));
}"""

UP = """([sel, id]) => {
  const el = document.querySelector(`[data-control="${sel}"]`);
  const r = el.getBoundingClientRect();
  const o = {pointerId:id, pointerType:'touch', isPrimary:id===1, bubbles:true, cancelable:true,
             clientX:r.x+r.width/2, clientY:r.y+r.height/2};
  el.dispatchEvent(new PointerEvent('pointerup', o));
}"""


async def types(page):
    return await page.evaluate("() => (window.__stemTape?.commands ?? []).map(c => c.type)")


async def clear(page):
    await page.evaluate("() => { if (window.__stemTape) window.__stemTape.commands.length = 0; }")


async def gesture(page, track_delay_ms, hold_ms):
    await clear(page)
    await page.evaluate(DOWN, ["play", 1])
    await asyncio.sleep(track_delay_ms / 1000)
    await page.evaluate(DOWN, ["track-button-1", 2])
    await asyncio.sleep(hold_ms / 1000)
    await page.evaluate(UP, ["track-button-1", 2])
    await page.evaluate(UP, ["play", 1])
    await asyncio.sleep(0.6)
    return await types(page)


async def main():
    results = {}
    async with async_playwright() as p:
        b = await p.chromium.launch(headless=True)
        ctx = await b.new_context(viewport={"width": 1280, "height": 1800}, has_touch=True)
        page = await ctx.new_page()
        await page.goto(URL, wait_until="domcontentloaded")
        await page.wait_for_selector('[data-control="play"]')
        await page.wait_for_function("() => !!window.__stemTape")

        # 1. Track at ~400 ms (inside the 450 ms claim window), released at
        #    ~250 ms overlap → solo latch, no transport, no global loop.
        results["claim_400ms_release_250ms"] = await gesture(page, 400, 250)

        # 2. Track at ~520 ms (past the claim window) → PLAY is owned by the
        #    global-loop hold; no chord may appear.
        results["late_520ms"] = await gesture(page, 520, 250)

        # 3. Track inside the window but held past 700 ms overlap → link.
        results["claim_200ms_hold_820ms"] = await gesture(page, 200, 820)

        await b.close()
    print(json.dumps(results, indent=2))

    ok = True
    solo = results["claim_400ms_release_250ms"]
    if "stem.solo" not in solo or any(t.startswith("transport.") or t.startswith("loop.global") for t in solo):
        ok = False
        print("FAIL: claimed chord did not latch solo cleanly", file=sys.stderr)
    if "stem.solo" in results["late_520ms"] or "stem.link" in results["late_520ms"]:
        ok = False
        print("FAIL: late Track press leaked a chord", file=sys.stderr)
    if "stem.link" not in results["claim_200ms_hold_820ms"]:
        ok = False
        print("FAIL: 820 ms overlap did not link", file=sys.stderr)
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


asyncio.run(main())
