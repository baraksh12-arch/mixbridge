// SessionManager.swift
// Orchestrates the full MixBridge connection lifecycle on iOS.
//
// State machine:
//   idle → discovered → connecting → configuring → streaming → (reconnecting)
//
// Owns: UDPReceiver, AudioEngine, JitterBuffer, BonjourBrowser
// Publishes state changes for SwiftUI observation.

import Foundation
import Network
import Combine
import UIKit

// MARK: - Session State

enum SessionState: Equatable {
    case idle
    case searching
    case connecting(host: String)
    case configuring
    case streaming(device: String)
    case reconnecting(device: String)

    var displayString: String {
        switch self {
        case .idle:                return "Not Connected"
        case .searching:           return "Searching…"
        case .connecting(let h):   return "Connecting to \(h)…"
        case .configuring:         return "Configuring…"
        case .streaming(let d):    return "Streaming from \(d)"
        case .reconnecting(let d): return "Reconnecting to \(d)…"
        }
    }

    var isStreaming: Bool {
        if case .streaming = self { return true }
        return false
    }

    var isConnected: Bool {
        switch self {
        case .streaming, .configuring: return true
        default: return false
        }
    }
}

// MARK: - Session Manager

@MainActor
final class SessionManager: ObservableObject {

    // MARK: Published

    @Published private(set) var sessionState: SessionState = .idle
    @Published private(set) var connectedComputerName: String = ""
    @Published private(set) var sampleRate: UInt32 = 0
    @Published private(set) var latencyMs: Double = 0
    @Published private(set) var packetLossPercent: Float = 0
    @Published private(set) var jitterMs: Double = 0
    @Published private(set) var networkQuality: NetworkQuality = .unknown
    @Published private(set) var jitterBufferMs: Double = 0
    @Published private(set) var signalPeakL: Float = 0
    @Published private(set) var signalPeakR: Float = 0
    @Published private(set) var autoReconnect: Bool = true
    @Published private(set) var discoveredSources: [DiscoveredSource] = []

    // Forwarded from AudioEngine
    var audioEngineState: AudioEngineState { audioEngine.state }
    var outputLatencyMs: Double { audioEngine.outputLatencyMs }

    // MARK: Private

    // Receive-thread subsystems (internal locking; accessed from UDP thread)
    nonisolated(unsafe) private let udpReceiver = UDPReceiver()
    nonisolated(unsafe) private let jitterBuffer = JitterBuffer()
    private let audioEngine: AudioEngine
    private let bonjourBrowser = BonjourBrowser()
    private let clockSync = ClockSynchronizer()

    // Connection tracking
    private var currentEndpoint: NWEndpoint?
    private var currentSessionId: UInt32 = 0
    private var sequenceCounter: UInt32  = 0
    private var lastHeartbeatNs: UInt64  = 0
    private var connectionStartNs: UInt64 = 0
    nonisolated(unsafe) private var lastPluginPacketNs: UInt64 = 0
    nonisolated(unsafe) private var cachedPluginHost: String = ""
    nonisolated(unsafe) private var cachedPluginPort: UInt16 = 0
    nonisolated(unsafe) private var cachedSessionId: UInt32 = 0
    nonisolated(unsafe) private var cachedSeqCounter: UInt32 = 0
    // Reconnection
    private var reconnectTimer: Timer?
    private var connectionTimeoutTimer: Timer?
    private var heartbeatTimer: Timer?

    // Preferences
    private let defaults = UserDefaults.standard
    private static let kLastHostKey       = "MixBridge.lastHost"
    private static let kLastPortKey       = "MixBridge.lastPort"
    private static let kAutoReconnectKey  = "MixBridge.autoReconnect"
    private static let kLatencyModeKey    = "MixBridge.latencyMode"

    private var preferredLatencyMode: JitterBuffer.Mode = .stable {
        didSet {
            jitterBuffer.mode = preferredLatencyMode
            defaults.set(preferredLatencyMode.rawValue,
                         forKey: Self.kLatencyModeKey)
        }
    }

    // Stats
    private var packetsSentFromPlugin: UInt64 = 0
    private var packetsReceivedLocal: UInt64   = 0
    private var packetsDroppedLocal: UInt64    = 0

    // Clock sync
    private var pendingClockSyncT2: UInt64 = 0

    // MARK: - Init

    init() {
        self.audioEngine = AudioEngine(jitterBuffer: jitterBuffer)

        // Restore preferences
        let savedMode = defaults.integer(forKey: Self.kLatencyModeKey)
        preferredLatencyMode = JitterBuffer.Mode(rawValue: savedMode) ?? .auto
        autoReconnect = defaults.bool(forKey: Self.kAutoReconnectKey)

        setupUDPReceiver()
        setupBonjourBrowser()
        setupBackgroundHandling()
    }

    // MARK: - Public API

    func startDiscovery() {
        guard sessionState == .idle else { return }
        sessionState = .searching
        bonjourBrowser.startBrowsing(publishPort: udpReceiver.boundPort)

        // Fallback: only try last-known host if Bonjour finds nothing after 6s
        if autoReconnect,
           defaults.string(forKey: Self.kLastHostKey) != nil,
           defaults.value(forKey: Self.kLastPortKey) != nil {
            DispatchQueue.main.asyncAfter(deadline: .now() + 6.0) { [weak self] in
                guard let self,
                      self.sessionState == .searching,
                      self.discoveredSources.isEmpty,
                      let lastHost = self.defaults.string(forKey: Self.kLastHostKey),
                      let lastPort = self.defaults.value(forKey: Self.kLastPortKey) as? UInt16
                else { return }
                self.connectToHost(lastHost, port: lastPort)
            }
        }
    }

    func stopDiscovery() {
        bonjourBrowser.stopBrowsing()
        if sessionState == .searching {
            sessionState = .idle
        }
    }

    func connectToSource(_ source: DiscoveredSource) {
        connectedComputerName = source.computerName
        connectToHost(source.address, port: source.port)
    }

    func connectToHost(_ host: String, port: UInt16) {
        let cleanHost = MixBridgeNetworkHelpers.cleanHostString(host)
        guard cleanHost.contains(".") else {
            return
        }
        guard let endpoint = makeEndpoint(host: cleanHost, port: port) else { return }

        // Don't restart handshake if already connected to this host
        if sessionState == .configuring || sessionState.isStreaming {
            if let ep = endpointHostPort(), ep.host == cleanHost, ep.port == port {
                return
            }
        }

        // Re-handshake from reconnecting: allow even if same host

        // Start receiver if not already running
        if !udpReceiver.isRunning {
            try? udpReceiver.start()
            bonjourBrowser.updatePublishPort(udpReceiver.boundPort)
        }

        currentEndpoint = endpoint
        connectionStartNs = HighResClock.nowNs()

        cachedPluginHost = cleanHost
        cachedPluginPort = port

        defaults.set(cleanHost, forKey: Self.kLastHostKey)
        defaults.set(port, forKey: Self.kLastPortKey)

        sendConnectHelloAck(to: cleanHost, port: port)
        sessionState = .configuring
        scheduleConnectionTimeout()
    }

    func disconnect() {
        sendGoodbye()
        cleanupSession()
        sessionState = .idle
        bonjourBrowser.stopBrowsing()
    }

    func setLatencyMode(_ mode: JitterBuffer.Mode) {
        preferredLatencyMode = mode
        // Notify plugin of mode change
        if case .streaming = sessionState {
            sendLatencyModeRequest(mode)
        }
    }

    func setAutoReconnect(_ enabled: Bool) {
        autoReconnect = enabled
        defaults.set(enabled, forKey: Self.kAutoReconnectKey)
    }

    // MARK: - Setup

    private func setupUDPReceiver() {
        udpReceiver.delegate = self
        try? udpReceiver.start()
    }

    private func setupBonjourBrowser() {
        bonjourBrowser.onSourceFound = { [weak self] source in
            Task { @MainActor [weak self] in
                guard let self else { return }
                if !self.discoveredSources.contains(where: { $0.name == source.name }) {
                    self.discoveredSources.append(source)
                }
                // Auto-connect only while searching — not during configuring/streaming
                switch self.sessionState {
                case .idle, .searching:
                    self.connectToSource(source)
                default:
                    break
                }
            }
        }

        bonjourBrowser.onSourceLost = { [weak self] name in
            Task { @MainActor [weak self] in
                self?.discoveredSources.removeAll { $0.name == name }
            }
        }
    }

    private func setupBackgroundHandling() {
        NotificationCenter.default.addObserver(
            forName: UIApplication.willResignActiveNotification,
            object: nil, queue: .main) { [weak self] _ in
                self?.handleAppBackground()
        }

        NotificationCenter.default.addObserver(
            forName: UIApplication.didBecomeActiveNotification,
            object: nil, queue: .main) { [weak self] _ in
                self?.handleAppForeground()
        }

        NotificationCenter.default.addObserver(
            forName: UIApplication.didEnterBackgroundNotification,
            object: nil, queue: .main) { [weak self] _ in
                // Audio continues in background via AVAudioSession .playback category
                // Connection monitoring continues on network thread
                _ = self // keep alive
        }
    }

    private func handleAppBackground() {
        // Keep audio session active - already configured for background playback
        // Keep UDP receiver running
        // Heartbeat timer remains active
    }

    private func handleAppForeground() {
        // If connection was lost while backgrounded, attempt reconnect
        if case .reconnecting = sessionState {
            attemptReconnect()
        }
    }

    // MARK: - Protocol Sending

    /// Path A: iOS initiates by sending HelloAck to discovered plugin.
    private func sendConnectHelloAck(to host: String, port: UInt16) {
        let sessionId = generateSessionId()
        currentSessionId = sessionId
        cachedSessionId  = sessionId
        cachedPluginHost = host
        cachedPluginPort = port
        cachedSeqCounter = sequenceCounter

        let pkt = udpReceiver.buildHelloAckPacket(
            sessionId: sessionId,
            seqNum: nextSeq(),
            latencyMode: UInt8(preferredLatencyMode.rawValue))
        udpReceiver.send(pkt, to: host, port: port)
    }

    private func sendHelloAck(to host: String, port: UInt16) {
        let pkt = udpReceiver.buildHelloAckPacket(
            sessionId: currentSessionId,
            seqNum: nextSeq(),
            latencyMode: UInt8(preferredLatencyMode.rawValue))
        udpReceiver.send(pkt, to: host, port: port)
    }

    private func sendSessionAck(to host: String, port: UInt16, accepted: Bool) {
        let jitterMs = UInt16(jitterBuffer.targetDepthMs)
        let pkt = udpReceiver.buildSessionAckPacket(
            sessionId: currentSessionId,
            seqNum: nextSeq(),
            accepted: accepted,
            latencyMode: UInt8(preferredLatencyMode.rawValue),
            jitterBufferMs: jitterMs)
        udpReceiver.send(pkt, to: host, port: port)
    }

    private func sendHeartbeatAck(to host: String, port: UInt16,
                                   originTs: UInt64) {
        let pkt = udpReceiver.buildHeartbeatAckPacket(
            sessionId: currentSessionId,
            seqNum: nextSeq(),
            originTs: originTs)
        udpReceiver.send(pkt, to: host, port: port)
    }

    private func sendClockSyncAck(to host: String, port: UInt16,
                                   t1: UInt64, t2: UInt64) {
        let pkt = udpReceiver.buildClockSyncAckPacket(
            sessionId: currentSessionId,
            seqNum: nextSeq(),
            t1: t1, t2: t2)
        udpReceiver.send(pkt, to: host, port: port)
    }

    private func sendGoodbye() {
        guard let ep = endpointHostPort() else { return }
        var data = Data(count: 20)
        data.withUnsafeMutableBytes { ptr in
            guard let base = ptr.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
            // Build minimal Goodbye packet
            let magic: UInt32 = 0x4D585247
            for i in 0..<4 { base[i] = UInt8((magic >> (i * 8)) & 0xFF) }
            base[4] = 1    // version
            base[5] = 0x03 // Goodbye
            base[6] = 0; base[7] = 0
            for i in 0..<4 { base[8 + i] = UInt8((nextSeq() >> (i * 8)) & 0xFF) }
            for i in 0..<4 { base[12 + i] = UInt8((currentSessionId >> (i * 8)) & 0xFF) }
            base[16] = 0 // reason = normal
        }
        udpReceiver.send(data, to: ep.host, port: ep.port)
    }

    private func sendLatencyModeRequest(_ mode: JitterBuffer.Mode) {
        guard let ep = endpointHostPort() else { return }
        var data = Data(count: 18)
        data.withUnsafeMutableBytes { ptr in
            guard let base = ptr.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
            let magic: UInt32 = 0x4D585247
            for i in 0..<4 { base[i] = UInt8((magic >> (i * 8)) & 0xFF) }
            base[4] = 1
            base[5] = 0x08 // LatencyMode packet type
            base[6] = 0; base[7] = 0
            for i in 0..<4 { base[8 + i] = UInt8((nextSeq() >> (i * 8)) & 0xFF) }
            for i in 0..<4 { base[12 + i] = UInt8((currentSessionId >> (i * 8)) & 0xFF) }
            base[16] = UInt8(mode.rawValue)
        }
        udpReceiver.send(data, to: ep.host, port: ep.port)
    }

    // MARK: - Timers

    private func scheduleConnectionTimeout() {
        connectionTimeoutTimer?.invalidate()
        connectionTimeoutTimer = Timer.scheduledTimer(
            withTimeInterval: 5.0, repeats: false) { [weak self] _ in
                Task { @MainActor [weak self] in
                    self?.handleConnectionTimeout()
                }
        }
    }

    private func handleConnectionTimeout() {
        switch sessionState {
        case .connecting, .configuring:
            defaults.removeObject(forKey: Self.kLastHostKey)
            defaults.removeObject(forKey: Self.kLastPortKey)
            sessionState = .searching
            scheduleReconnect()
        default:
            break
        }
    }

    private func startHeartbeatMonitor() {
        heartbeatTimer?.invalidate()
        heartbeatTimer = Timer.scheduledTimer(
            withTimeInterval: 1.5, repeats: true) { [weak self] _ in
                Task { @MainActor [weak self] in
                    self?.checkHeartbeatTimeout()
                }
        }
    }

    private func checkHeartbeatTimeout() {
        guard case .streaming(let device) = sessionState else { return }
        let elapsed = Double(HighResClock.nowNs() - lastPluginPacketNs) / 1_000_000.0
        if elapsed > 20000 {
            sessionState = .reconnecting(device: device)
            scheduleReconnect()
        } else if elapsed > 12000 {
            // Soft recovery: nudge plugin without tearing down audio
            if let ep = endpointHostPort() {
                sendHelloAck(to: ep.host, port: ep.port)
            }
        }
    }

    private func scheduleReconnect() {
        guard autoReconnect else { return }
        reconnectTimer?.invalidate()
        reconnectTimer = Timer.scheduledTimer(
            withTimeInterval: 2.0, repeats: false) { [weak self] _ in
                Task { @MainActor [weak self] in
                    self?.attemptReconnect()
                }
        }
    }

    private func attemptReconnect() {
        guard let lastHost = defaults.string(forKey: Self.kLastHostKey),
              let lastPort = defaults.value(forKey: Self.kLastPortKey) as? UInt16
        else {
            sessionState = .searching
            bonjourBrowser.startBrowsing(publishPort: udpReceiver.boundPort)
            return
        }
        cachedPluginHost = lastHost
        cachedPluginPort = lastPort

        if currentSessionId != 0 {
            // Reuse existing session — plugin will refresh, not full reset
            sendHelloAck(to: lastHost, port: lastPort)
            sessionState = .configuring
            scheduleConnectionTimeout()
        } else {
            connectToHost(lastHost, port: lastPort)
        }
    }

    // MARK: - Session Cleanup

    private func cleanupSession() {
        heartbeatTimer?.invalidate()
        heartbeatTimer = nil
        reconnectTimer?.invalidate()
        reconnectTimer = nil
        connectionTimeoutTimer?.invalidate()
        connectionTimeoutTimer = nil
        currentEndpoint   = nil
        currentSessionId  = 0
        sequenceCounter   = 0
        cachedPluginHost  = ""
        cachedPluginPort  = 0
        cachedSessionId   = 0
        cleanupAudio()
        jitterBuffer.reset()
        clockSync.reset()
        connectedComputerName = ""
        sampleRate = 0
    }

    private func cleanupAudio() {
        audioEngine.stop()
        jitterBuffer.reset()
    }

    // MARK: - Helpers

    private func nextSeq() -> UInt32 {
        let s = sequenceCounter
        sequenceCounter &+= 1
        cachedSeqCounter = sequenceCounter
        return s
    }

    private func generateSessionId() -> UInt32 {
        return UInt32.random(in: 1..<UInt32.max)
    }

    private func makeEndpoint(host: String, port: UInt16) -> NWEndpoint? {
        guard let nwPort = NWEndpoint.Port(rawValue: port) else { return nil }
        return .hostPort(host: NWEndpoint.Host(host), port: nwPort)
    }

    private func endpointHostPort() -> (host: String, port: UInt16)? {
        guard let ep = currentEndpoint,
              case .hostPort(let host, let port) = ep else { return nil }
        return (MixBridgeNetworkHelpers.cleanHostString(host.debugDescription), port.rawValue)
    }
}

// MARK: - UDPReceiverDelegate

extension SessionManager: UDPReceiverDelegate {

    nonisolated func didReceiveHello(senderTimestampNs: UInt64,
                                      pluginName: String,
                                      computerName: String,
                                      fromEndpoint: NWEndpoint) {
        let receivedAt = HighResClock.nowNs()

        Task { @MainActor [weak self] in
            guard let self else { return }

            // Path A may already have sent HelloAck; ignore duplicate Hello
            switch self.sessionState {
            case .configuring, .streaming:
                return
            default:
                break
            }

            guard case .hostPort(let host, let port) = fromEndpoint else { return }
            let hostStr = MixBridgeNetworkHelpers.cleanHostString(host.debugDescription)
            let portVal = port.rawValue

            // Save for reconnection
            self.defaults.set(hostStr, forKey: Self.kLastHostKey)
            self.defaults.set(portVal, forKey: Self.kLastPortKey)

            self.currentEndpoint = fromEndpoint
            self.connectedComputerName = computerName
            self.lastPluginPacketNs    = receivedAt
            self.cachedPluginHost      = hostStr
            self.cachedPluginPort      = portVal

            // Generate session ID
            self.currentSessionId = self.generateSessionId()
            self.cachedSessionId  = self.currentSessionId
            self.cachedSeqCounter = self.sequenceCounter

            // Send HelloAck
            self.sendHelloAck(to: hostStr, port: portVal)

            self.connectionTimeoutTimer?.invalidate()
            self.sessionState = .configuring
        }
    }

    nonisolated func didReceiveSessionConfig(sessionId: UInt32,
                                              sampleRate: UInt32,
                                              blockSize: UInt16,
                                              framesPerPacket: UInt32,
                                              bitrateKbps: UInt32,
                                              latencyMode: UInt8) {
        let receivedAt = HighResClock.nowNs()

        Task { @MainActor [weak self] in
            guard let self else { return }
            guard let ep = self.endpointHostPort() else { return }

            self.currentSessionId = sessionId
            self.cachedSessionId  = sessionId
            self.sampleRate         = sampleRate
            self.lastPluginPacketNs = receivedAt

            do {
                try self.audioEngine.configure(
                    sampleRate: Double(sampleRate),
                    framesPerPacket: Int(framesPerPacket))

                // Send SessionAck
                self.sendSessionAck(to: ep.host, port: ep.port, accepted: true)
                    self.connectionTimeoutTimer?.invalidate()
                self.sessionState = .streaming(device: self.connectedComputerName)
                self.startHeartbeatMonitor()

            } catch {
                self.sendSessionAck(to: ep.host, port: ep.port, accepted: false)
                self.cleanupSession()
                self.sessionState = .idle
            }
        }
    }

    nonisolated func didReceiveAudioFrame(sequenceNumber: UInt32,
                                           captureTimestampNs: UInt64,
                                           sampleRate: UInt32,
                                           frameCount: UInt32,
                                           sampleOffset: UInt64,
                                           peakL: Float, peakR: Float,
                                           samples: [Float],
                                           payloadCRC: UInt32) {
        let now = HighResClock.nowNs()

        // Push to jitter buffer immediately (on receive thread)
        let packet = BufferedPacket(
            sequenceNumber:    sequenceNumber,
            captureTimestampNs: captureTimestampNs,
            sampleOffset:      sampleOffset,
            sampleRate:        sampleRate,
            frameCount:        frameCount,
            samples:           samples)

        jitterBuffer.push(packet)

        lastPluginPacketNs = now

        Task { @MainActor [weak self] in
            guard let self else { return }
            self.packetsReceivedLocal += 1
            self.signalPeakL = peakL
            self.signalPeakR = peakR
            self.jitterBufferMs = self.jitterBuffer.currentDepthMs
        }
    }

    nonisolated func didReceiveHeartbeat(senderTimestampNs: UInt64,
                                          signalL: Float, signalR: Float,
                                          fromEndpoint: NWEndpoint) {
        let now = HighResClock.nowNs()
        lastPluginPacketNs = now

        if case .hostPort(let host, let port) = fromEndpoint {
            cachedPluginHost = MixBridgeNetworkHelpers.cleanHostString(host.debugDescription)
            cachedPluginPort = port.rawValue
        }

        let host = cachedPluginHost
        let port = cachedPluginPort
        let sessionId = cachedSessionId
        if !host.isEmpty && port > 0 {
            let seq = cachedSeqCounter
            cachedSeqCounter &+= 1
            let pkt = udpReceiver.buildHeartbeatAckPacket(
                sessionId: sessionId,
                seqNum: seq,
                originTs: senderTimestampNs)
            udpReceiver.send(pkt, to: host, port: port)
        }

        Task { @MainActor [weak self] in
            guard let self else { return }
            self.lastHeartbeatNs = now
        }
    }

    nonisolated func didReceiveClockSync(t1Ns: UInt64, fromEndpoint: NWEndpoint) {
        let t2 = HighResClock.nowNs()
        lastPluginPacketNs = t2

        let host = cachedPluginHost
        let port = cachedPluginPort
        let sessionId = cachedSessionId
        if !host.isEmpty && port > 0 {
            let seq = cachedSeqCounter
            cachedSeqCounter &+= 1
            let pkt = udpReceiver.buildClockSyncAckPacket(
                sessionId: sessionId,
                seqNum: seq,
                t1: t1Ns, t2: t2)
            udpReceiver.send(pkt, to: host, port: port)
        }
    }

    nonisolated func didReceiveGoodbye() {
        Task { @MainActor [weak self] in
            guard let self else { return }
            self.cleanupSession()

            if self.autoReconnect {
                self.sessionState = .reconnecting(device: self.connectedComputerName)
                self.scheduleReconnect()
            } else {
                self.sessionState = .idle
            }
        }
    }

    nonisolated func didReceiveStatsResponse(_ stats: StatsSnapshot) {
        Task { @MainActor [weak self] in
            guard let self else { return }
            self.latencyMs          = Double(stats.latencyMs)
            self.packetLossPercent  = stats.lossPercent
            self.jitterMs           = Double(stats.jitterMs)
            self.networkQuality     = NetworkQuality(rawValue: stats.networkQuality)
                                      ?? .unknown
        }
    }
}

// MARK: - NetworkQuality

enum NetworkQuality: UInt8 {
    case unknown   = 0
    case excellent = 1
    case good      = 2
    case fair      = 3
    case poor      = 4

    var displayString: String {
        switch self {
        case .unknown:   return "—"
        case .excellent: return "Excellent"
        case .good:      return "Good"
        case .fair:      return "Fair"
        case .poor:      return "Poor"
        }
    }

    var color: String {  // SwiftUI Color name
        switch self {
        case .unknown:              return "gray"
        case .excellent, .good:    return "green"
        case .fair:                 return "orange"
        case .poor:                 return "red"
        }
    }
}

// MARK: - DiscoveredSource

struct DiscoveredSource: Identifiable, Equatable {
    let id = UUID()
    let name: String
    let address: String
    let port: UInt16
    let computerName: String

    static func == (lhs: DiscoveredSource, rhs: DiscoveredSource) -> Bool {
        lhs.name == rhs.name
    }
}

// MARK: - ClockSynchronizer (Swift)

final class ClockSynchronizer {
    private var offsetNs: Int64  = 0
    private var rttNs: Int64     = 0
    private var jitterNs: Int64  = 0
    private var sampleCount: Int = 0

    func processAck(t1: UInt64, t2: UInt64, t3: UInt64) {
        let t4 = HighResClock.nowNs()
        guard t4 >= t1, t2 >= t1, t3 >= t2 else { return }

        let rtt    = Int64((t4 - t1) - (t3 - t2))
        let offset = (Int64(t2 - t1) + Int64(t3) - Int64(t4)) / 2

        guard rtt >= 0 && rtt < 500_000_000 else { return }

        if sampleCount == 0 {
            offsetNs = offset
            rttNs    = rtt
        } else {
            offsetNs = offsetNs + (offset - offsetNs) / 8
            rttNs    = rttNs    + (rtt    - rttNs)    / 8
        }
        let dev = abs(rtt - rttNs)
        jitterNs = jitterNs + (dev - jitterNs) / 8
        sampleCount += 1
    }

    var rttMs:    Double { Double(rttNs) / 1_000_000.0 }
    var jitterMs: Double { Double(jitterNs) / 1_000_000.0 }
    var isValid:  Bool   { sampleCount >= 3 }

    func reset() {
        offsetNs = 0; rttNs = 0; jitterNs = 0; sampleCount = 0
    }
}

// MARK: - JitterBuffer.Mode RawRepresentable

extension JitterBuffer.Mode: RawRepresentable {
    typealias RawValue = Int

    init?(rawValue: Int) {
        switch rawValue {
        case 0: self = .auto
        case 1: self = .ultraLow
        case 2: self = .low
        case 3: self = .normal
        case 4: self = .stable
        default: return nil
        }
    }

    var rawValue: Int {
        switch self {
        case .auto:     return 0
        case .ultraLow: return 1
        case .low:      return 2
        case .normal:   return 3
        case .stable:   return 4
        }
    }

    var displayName: String {
        switch self {
        case .auto:     return "Auto"
        case .ultraLow: return "Ultra Low"
        case .low:      return "Low"
        case .normal:   return "Normal"
        case .stable:   return "Stable"
        }
    }
}
