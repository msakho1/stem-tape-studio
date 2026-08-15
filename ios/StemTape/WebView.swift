import SwiftUI
import WebKit

/// Owns the WKWebView and the CoreMIDI input, and performs the page handshake.
final class WebHost: NSObject, ObservableObject, WKNavigationDelegate {
    let midi = MidiBridge()
    private(set) var webView: WKWebView?

    func attach(_ webView: WKWebView) {
        self.webView = webView
        webView.navigationDelegate = self
        midi.onBatch = { [weak self] events in self?.push(events) }
        midi.onDisconnect = { [weak self] info in self?.callBridge("disconnect", arg: info) }
        midi.start()
    }

    // MARK: page handshake

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        handshake()
    }

    /// window.__stemTapeMidi.ready({deviceName}) returns the page's
    /// performance.now() value; pairing it with a locally sampled
    /// performance-clock reading anchors native timestamps to the page clock.
    func handshake() {
        let nativeNow = MidiBridge.nowMs()
        let info: [String: Any] = ["deviceName": midi.currentDeviceName ?? NSNull()]
        webView?.callAsyncJavaScript(
            "return (window.__stemTapeMidi && window.__stemTapeMidi.ready(info)) || null;",
            arguments: ["info": info],
            in: nil,
            in: .page
        ) { [weak self] result in
            guard case .success(let value) = result,
                  let dict = value as? [String: Any],
                  let pageNow = dict["perfNowMs"] as? Double else { return }
            // Native perf clock -> page perf clock offset.
            self?.midi.setPageClockOffset(pageNow - nativeNow)
        }
    }

    // MARK: delivery

    private func push(_ events: [[String: Any]]) {
        guard !events.isEmpty else { return }
        webView?.callAsyncJavaScript(
            "return (window.__stemTapeMidi && window.__stemTapeMidi.push(events)) || 0;",
            arguments: ["events": events],
            in: nil,
            in: .page,
            completionHandler: nil
        )
    }

    private func callBridge(_ fn: String, arg: [String: Any]) {
        webView?.callAsyncJavaScript(
            "window.__stemTapeMidi && window.__stemTapeMidi.\(fn)(info);",
            arguments: ["info": arg],
            in: nil,
            in: .page,
            completionHandler: nil
        )
    }
}

struct WebView: UIViewRepresentable {
    let host: WebHost
    let url: URL

    func makeUIView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.allowsInlineMediaPlayback = true
        config.mediaTypesRequiringUserActionForPlayback = []
        let webView = WKWebView(frame: .zero, configuration: config)
        webView.isOpaque = false
        webView.scrollView.bounces = false
        webView.scrollView.contentInsetAdjustmentBehavior = .never
        if #available(iOS 16.4, *) { webView.isInspectable = true }
        host.attach(webView)
        webView.load(URLRequest(url: url))
        return webView
    }

    func updateUIView(_ uiView: WKWebView, context: Context) {}
}
