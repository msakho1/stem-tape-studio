# Stem Tape iOS wrapper (CoreMIDI → web bridge)

Full-screen `WKWebView` shell that feeds wired class-compliant USB MIDI into the
existing web bridge (`src/audio/midi/nativeBridge.ts`).

## Build

```
brew install xcodegen
cd ios/StemTape
xcodegen generate
xcodebuild -project StemTape.xcodeproj -scheme StemTape \
  -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build
```

Set `STEM_TAPE_WEB_URL` in `Config.xcconfig` (or pass it on the `xcodebuild`
command line). No domain is compiled into Swift; when the setting is missing the
app shows a configuration message instead of loading anything.

## Behaviour

- `AVAudioSession` category `.playback` → Silent switch does not mute audio.
- One `MIDIInputPortCreateWithProtocol` port; all sources connected, and
  `msgObjectAdded` / `msgObjectRemoved` / `msgSetupChanged` handle hot-plug.
- Note On, Note Off, velocity-zero Note On → `noteOff`, CC123 → `allNotesOff`.
- Batches are delivered with structured `callAsyncJavaScript` arguments to
  `window.__stemTapeMidi.push(events)` — never string interpolation.
- Handshake on `didFinish`: `__stemTapeMidi.ready({deviceName})` returns the
  page's `performance.now()`; the difference against the natively sampled clock
  becomes the timestamp offset applied to every event.
- Disconnect and app backgrounding call `__stemTapeMidi.disconnect(info)`, which
  the web side normalizes into `allNotesOff`.
