# Stem Tape companion v1.1 — production-build audit

Scope: mock exclusion, browser audio-preparation path, `/device` smoke, lint,
tests, typecheck, and handoff-manifest validation. No feature changes, no
firmware changes, no repository-wide `eslint --fix`.

## 1. Production build

    $ bun run build          # vite build -> nitro cloudflare-module
    dist/client/assets/routes-Chca1JeP.js   420.17 kB │ gzip: 120.53 kB
    dist/client/assets/index-BzxhOQqA.js    347.21 kB │ gzip: 109.14 kB
    dist/client/assets/device-9DDQOVxl.js    60.56 kB │ gzip:  18.55 kB
    ✓ built in 2.07s (client) / 1.22s (ssr)
    EXIT=0

Served for the audit with the real production server:

    $ cd dist && npx wrangler dev --port 4173 --ip 127.0.0.1 --local
    [wrangler:info] Ready on http://127.0.0.1:4173
    [wrangler:info] GET / 200 OK

## 2. Mock exclusion (static)

Source gate (`src/routes/device.tsx:114`):

    const injected = import.meta.env.DEV
      ? (globalThis as { __SP1_MOCK_PORT__?: SerialLikePort }).__SP1_MOCK_PORT__
      : undefined;

Grep of the emitted bundles:

    __SP1_MOCK_PORT__      dist/client [0 hits]   dist/server [0 hits]
    MockSp1                dist/client [0 hits]   dist/server [0 hits]
    mockSerial             dist/client [0 hits]   dist/server [0 hits]
    import.meta.env.DEV    dist/client [0 hits]
    fixtures/              dist/client [0 hits]
    *.wav under dist/                  [none]

Only the dead UI string literal `SIMULATED DEVICE — nothing is written to
hardware` survives, inside `device-*.js`; it is reachable solely through
`setMockMode(!!injected)` (`device.tsx:145`), whose only input is the
eliminated branch.

## 3. Mock exclusion (runtime, Chromium against the production server)

The injected global is planted on the production origin and carried into
`/device` by client-side navigation, then Connect is clicked:

    url                              http://127.0.0.1:4173/device
    injected_global_present_on_device true      <- attacker global survives
    connect_button_clicked            true
    simulated_badge_count             0
    has_SIMULATED_text                false
    write_state                       "no data has been written"
    console_errors                    []
    unhandled_rejections              []

A published page cannot be pushed into simulated-device mode from the console.

## 4. Audio preparation through real browser audio APIs

Harness `tools/prepare-harness` (audit-only, not shipped) imports the
unmodified production modules `src/sp1/song.ts`, `src/sp1/sector.ts`,
`src/sp1/digest.ts`, is built with `vite build --mode production`, and runs in
headless Chromium with the real `AudioContext.decodeAudioData` and the real
`OfflineAudioContext`. Node reference values come from the same production
modules with the deterministic stub (`scripts/prepare-expected.ts`).

Structural results, browser vs node:

    schemaVersion        stem-tape-song/1   == stem-tape-song/1   MATCH
    sampleRate           48000              == 48000              MATCH
    channels / pcmDepth  2 / 24             == 2 / 24             MATCH
    frames               14592              == 14592              MATCH
    durationSeconds      0.304              == 0.304              MATCH
    audioBytes           350208             == 350208             MATCH
    lengthSpreadSeconds  0.054              == 0.054              MATCH
    sectorCount          43                 == 43                 MATCH
    sectorBytes          352256             == 352256             MATCH
    roundTripFrames      14592              == 14592              MATCH
    roundTripBpm         120                == 120                MATCH

Per stem, `decodeSectors(encodeSong(song))` reproduces the packed PCM exactly:

    vocal       roundtrip sectors->stem IDENTITY OK
    drums       roundtrip sectors->stem IDENTITY OK
    bass        roundtrip sectors->stem IDENTITY OK
    instrument  roundtrip sectors->stem IDENTITY OK

Sample bytes are NOT hash-identical across runtimes and must not be frozen as
such: Chromium's WAV decoder and its resampler are not the node stub. Measured
peak deltas on the non-resampled stems are ~1.5e-05 and ~1.9e-05 (below one
16-bit LSB, 3.05e-05), i.e. decoder rounding, not packing error. When the
harness forces a 48 kHz `AudioContext`, Chromium resamples 44.1 kHz sources at
decode time, so `CanonicalStem.source.sampleRate` reports the decode-context
rate; the `/device` UI does not rely on that field — it sniffs the true rate
from the RIFF header (`sniffHeader`) and displayed 44100 Hz correctly.

No network egress: the only requests the harness issued were its own
`index.html`, its JS chunk, and the four local fixture WAVs.

## 5. `/device` smoke on the production server

Four fixture WAVs uploaded through the real UI, BPM 120, beat zero 0:

    stages           1·CONNECT SP-1  2·ADD STEMS  3·PREPARE SONG  4·TRANSFER  5·VERIFY
    stem info        vocal.wav 48000 Hz 2 ch 304 ms
                     drums.wav 44100 Hz 2 ch 295 ms
                     bass.wav  48000 Hz 1 ch 292 ms
                     instrument.wav 44100 Hz 1 ch 250 ms
    prepared         untitled — 304 ms, 120 BPM, beat zero at 0s.
                     Four stems, 48 kHz stereo 24-bit.
    technical detail frames · 14592 @ 48000 Hz
                     audio bytes · 350208
                     sector bytes · 8192 (16 × 512 B blocks)
                     blocks to write · 688
                     song sha-256 · bd99482a9aeb22a869119ecb77c3c3af2ef249550335ce4f8a43399b0b775e90
                     vocal pad 0 · drums pad 442 · bass pad 592 · instrument pad 2592
    upload button    disabled
    upload lock      "…does not report Stem Tape v1.1 A/B firmware. It stays
                     read-only: no initialization, no writes, no delete."
    write state      "no data has been written"
    safe to disconnect  "safe to disconnect"
    POST/PUT/PATCH requests   []        <- no audio leaves the device
    console errors / page errors   [] / []

Padding, frame count, audio bytes and sector count produced by the UI path
match the node reference exactly (14592 frames, 350208 bytes, 43 sectors,
688 blocks, pads 0/442/592/2592).

Open wording defect (not changed here): with no device connected the capacity
line reads `43 sectors of ? available in the inactive staging slot · does NOT
fit — no data will be written`. The state is fail-safe (upload disabled), but
"does NOT fit" is asserted when capacity is unknown.

## 6. Lint, tests, typecheck, manifests

    $ npx eslint <7 audit files>            42 prettier errors -> --fix on those files only
    $ npx eslint <7 audit files>            EXIT=0, 0 problems
    $ bunx vitest run                       Test Files 45 passed (45)
                                            Tests 472 passed (472)   EXIT=0
    $ bunx vitest run src/sp1               Test Files 12 passed (12)
    $ npx tsgo --noEmit                     EXIT=0, no diagnostics
    $ sha256sum -c handoff/v1.1/SHA256SUMS.txt   20 listed / 20 OK / 0 failed
