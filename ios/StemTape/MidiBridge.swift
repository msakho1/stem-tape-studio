import Foundation
import CoreMIDI
import QuartzCore

/// Wired class-compliant USB MIDI in through ONE CoreMIDI input port.
///
/// Emits dictionaries that match the web `StemMidiEvent` shape consumed by
/// `src/audio/midi/nativeBridge.ts` (`normalizeNativeEvent`):
///   { kind, note, velocity, channel, timestampMs, deviceId, deviceName }
/// Velocity-zero Note On is normalized to noteOff here as well as on the web
/// side; CC123 becomes allNotesOff.
final class MidiBridge {
    var onBatch: (([[String: Any]]) -> Void)?
    var onDisconnect: (([String: Any]) -> Void)?

    private var client = MIDIClientRef()
    private var inputPort = MIDIPortRef()
    private var connected: [MIDIEndpointRef: (id: String, name: String)] = [:]
    private var pageClockOffsetMs: Double = 0
    private let queue = DispatchQueue(label: "stemtape.midi")

    private(set) var currentDeviceName: String?

    static func nowMs() -> Double { CACurrentMediaTime() * 1000.0 }

    func setPageClockOffset(_ offset: Double) {
        queue.sync { pageClockOffsetMs = offset }
    }

    func start() {
        let name = "StemTape" as CFString
        MIDIClientCreateWithBlock(name, &client) { [weak self] notification in
            self?.handleNotification(notification)
        }
        MIDIInputPortCreateWithProtocol(client, "StemTape In" as CFString, ._1_0, &inputPort) { [weak self] eventList, _ in
            self?.handleEventList(eventList)
        }
        connectAllSources()
    }

    // MARK: sources

    private func connectAllSources() {
        for i in 0..<MIDIGetNumberOfSources() {
            connect(MIDIGetSource(i))
        }
    }

    private func connect(_ endpoint: MIDIEndpointRef) {
        guard endpoint != 0, connected[endpoint] == nil else { return }
        guard MIDIPortConnectSource(inputPort, endpoint, nil) == noErr else { return }
        let info = (id: String(endpoint), name: displayName(endpoint))
        connected[endpoint] = info
        currentDeviceName = info.name
    }

    private func disconnect(_ endpoint: MIDIEndpointRef) {
        guard let info = connected.removeValue(forKey: endpoint) else { return }
        MIDIPortDisconnectSource(inputPort, endpoint)
        currentDeviceName = connected.values.first?.name
        // A yanked cable must not leave held notes ringing.
        onDisconnect?(["deviceId": info.id, "deviceName": info.name])
    }

    /// Explicit panic used on backgrounding.
    func panic() {
        onDisconnect?([
            "deviceId": connected.values.first?.id ?? "coremidi",
            "deviceName": currentDeviceName ?? "CoreMIDI device",
        ])
    }

    private func handleNotification(_ notification: UnsafePointer<MIDINotification>) {
        switch notification.pointee.messageID {
        case .msgObjectAdded:
            notification.withMemoryRebound(to: MIDIObjectAddRemoveNotification.self, capacity: 1) { n in
                if n.pointee.childType == .source { connect(n.pointee.child) }
            }
        case .msgObjectRemoved:
            notification.withMemoryRebound(to: MIDIObjectAddRemoveNotification.self, capacity: 1) { n in
                if n.pointee.childType == .source { disconnect(n.pointee.child) }
            }
        case .msgSetupChanged:
            connectAllSources()
        default:
            break
        }
    }

    private func displayName(_ endpoint: MIDIEndpointRef) -> String {
        var param: Unmanaged<CFString>?
        if MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &param) == noErr,
           let value = param?.takeRetainedValue() as String? {
            return value
        }
        return "CoreMIDI device"
    }

    // MARK: events

    private func handleEventList(_ eventList: UnsafePointer<MIDIEventList>) {
        let timestampMs = MidiBridge.nowMs() + pageClockOffsetMs
        let deviceId = connected.values.first?.id ?? "coremidi"
        let deviceName = currentDeviceName ?? "CoreMIDI device"
        var batch: [[String: Any]] = []

        var packet = eventList.pointee.packet
        for _ in 0..<eventList.pointee.numPackets {
            withUnsafeBytes(of: packet.words) { raw in
                let words = raw.bindMemory(to: UInt32.self)
                for w in 0..<Int(packet.wordCount) {
                    let word = words[w]
                    // MIDI 1.0 channel-voice UMP: message type 0x2.
                    guard (word >> 28) == 0x2 else { continue }
                    let status = UInt8((word >> 16) & 0xFF)
                    let d1 = UInt8((word >> 8) & 0x7F)
                    let d2 = UInt8(word & 0x7F)
                    if let event = Self.event(status: status, d1: d1, d2: d2,
                                              timestampMs: timestampMs,
                                              deviceId: deviceId, deviceName: deviceName) {
                        batch.append(event)
                    }
                }
            }
            packet = MIDIEventPacketNext(&packet).pointee
        }

        guard !batch.isEmpty else { return }
        let payload = batch
        DispatchQueue.main.async { [weak self] in self?.onBatch?(payload) }
    }

    /// Pure byte -> StemMidiEvent mapping (also used by the smoke check).
    static func event(status: UInt8, d1: UInt8, d2: UInt8,
                      timestampMs: Double, deviceId: String, deviceName: String) -> [String: Any]? {
        let type = status & 0xF0
        let channel = Int(status & 0x0F)
        func make(_ kind: String, _ note: Int, _ velocity: Int) -> [String: Any] {
            ["kind": kind, "note": note, "velocity": velocity, "channel": channel,
             "timestampMs": timestampMs, "deviceId": deviceId, "deviceName": deviceName]
        }
        switch type {
        case 0x90:
            return d2 == 0 ? make("noteOff", Int(d1), 0) : make("noteOn", Int(d1), Int(d2))
        case 0x80:
            return make("noteOff", Int(d1), Int(d2))
        case 0xB0 where d1 == 123:
            return make("allNotesOff", 0, 0)
        default:
            return nil
        }
    }
}
