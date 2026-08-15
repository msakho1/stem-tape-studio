import SwiftUI
import AVFoundation

@main
struct StemTapeApp: App {
    @Environment(\.scenePhase) private var scenePhase
    @StateObject private var host = WebHost()

    init() {
        configureAudioSession()
    }

    var body: some Scene {
        WindowGroup {
            RootView(host: host)
                .ignoresSafeArea()
                .onChange(of: scenePhase) { phase in
                    if phase != .active {
                        // Backgrounding must never leave a key stuck down.
                        host.midi.panic()
                    }
                }
        }
    }

    /// `.playback` keeps audio alive with the Silent switch engaged.
    private func configureAudioSession() {
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
        try? session.setPreferredIOBufferDuration(0.005)
        try? session.setActive(true)
    }
}

struct RootView: View {
    @ObservedObject var host: WebHost

    var body: some View {
        if let url = WebConfig.url {
            WebView(host: host, url: url)
        } else {
            ConfigurationMessage()
        }
    }
}

/// Shown when the `STEM_TAPE_WEB_URL` build setting is missing or unparsable.
struct ConfigurationMessage: View {
    var body: some View {
        VStack(spacing: 12) {
            Text("Stem Tape is not configured")
                .font(.headline)
            Text("Set STEM_TAPE_WEB_URL in Config.xcconfig (or the Xcode build setting) to the address of your Stem Tape site, then rebuild.")
                .font(.footnote)
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
        }
        .padding(32)
    }
}

/// No domain is compiled in: the URL comes from the build setting, surfaced
/// through Info.plist as `STEMTapeWebURL`.
enum WebConfig {
    static var url: URL? {
        guard let raw = Bundle.main.object(forInfoDictionaryKey: "STEMTapeWebURL") as? String else { return nil }
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, !trimmed.contains("$("), let url = URL(string: trimmed), url.scheme != nil else { return nil }
        return url
    }
}
