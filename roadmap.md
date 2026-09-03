# Roadmap

## In progress — master scratch correction (S3 rework)
- [ ] Fix doubled/echo audio: single in-flight migration + retire every stale node voice at worklet takeover
- [ ] Replace absolute rocker displacement with pointer hand-velocity → signed master velocity
- [ ] Hand-stop: held-still pointer decays commanded velocity to 0 within a short tunable timeout
- [ ] One rocker drag owner; capture survives crossing centre and leaving visual bounds
- [ ] Visual rocker motion decoupled from audio velocity
- [ ] Tests: stale node retirement, held-still → 0, reversal through zero, continuous (non-retriggered) reversal
- [ ] Full vitest + tsgo + build, then stop for browser acceptance

## Explicitly out of scope this pass
- Isolated stem (FUNCTION + fader) scratch — still legacy granular `laneFaderScrub`; NOT complete
- Loop / FX / LED / unrelated UI changes
