// BonjourBrowser.swift
// Discovers MixBridge VST plugin on the local network via Bonjour/mDNS.
// Also publishes this device as a receiver so the plugin can find us (Path B).
// Uses NWBrowser + NetService for publish. Fallback UDP broadcast when mDNS blocked.

import Foundation
import Network
import UIKit

// MARK: - BonjourBrowser

final class BonjourBrowser: NSObject, NetServiceDelegate {

    // MARK: Callbacks

    var onSourceFound: ((DiscoveredSource) -> Void)?
    var onSourceLost:  ((String) -> Void)?

    // MARK: Private

    private var browser: NWBrowser?
    private var publishedService: NetService?
    private var publishPort: Int32 = 51234
    private var fallbackSocket: Int32 = -1
    private var fallbackThread: Thread?
    private var isBrowsing = false
    private let serviceType = "_mixbridge._udp."
    private var knownSources: [String: DiscoveredSource] = [:]
    private let lock = NSLock()

    // MARK: - Public API

    func startBrowsing(publishPort: UInt16 = 51234) {
        guard !isBrowsing else { return }
        isBrowsing = true
        self.publishPort = Int32(publishPort)
        startPublishing()
        startNWBrowser()
        startFallbackBroadcastListener()
    }

    func updatePublishPort(_ port: UInt16) {
        guard Int32(port) != publishPort else { return }
        publishPort = Int32(port)
        if isBrowsing {
            stopPublishing()
            startPublishing()
        }
    }

    func stopBrowsing() {
        isBrowsing = false
        stopPublishing()
        browser?.cancel()
        browser = nil
        stopFallbackListener()
    }

    // MARK: - Bonjour Publish (Path B — plugin discovers iPhone)

    private func startPublishing() {
        stopPublishing()
        let deviceName = UIDevice.current.name
            .replacingOccurrences(of: " ", with: "-")
        let instanceName = "MixBridge-\(deviceName)"
        let service = NetService(
            domain: "local.",
            type: serviceType,
            name: instanceName,
            port: publishPort)
        service.delegate = self
        // NetService requires a run loop; publish() alone is a no-op without this.
        DispatchQueue.main.async {
            service.schedule(in: .main, forMode: .common)
            service.publish()
        }
        publishedService = service
    }

    private func stopPublishing() {
        publishedService?.stop()
        publishedService?.delegate = nil
        publishedService = nil
    }

    // MARK: - NWBrowser (Bonjour)

    private func startNWBrowser() {
        let params = NWParameters.udp
        params.includePeerToPeer = true

        let b = NWBrowser(for: .bonjourWithTXTRecord(type: serviceType, domain: "local."),
                          using: params)

        b.browseResultsChangedHandler = { [weak self] _, changes in
            self?.handleBrowseChanges(changes)
        }

        b.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:
                break
            case .failed:
                self?.browser = nil
            case .cancelled:
                self?.browser = nil
            default:
                break
            }
        }

        b.start(queue: DispatchQueue(label: "MixBridge.Bonjour",
                                      qos: .userInitiated))
        browser = b
    }

    private func handleBrowseChanges(_ changes: Set<NWBrowser.Result.Change>) {
        for change in changes {
            switch change {
            case .added(let result):
                handleResultAdded(result)
            case .removed(let result):
                handleResultRemoved(result)
            case .changed(_, let new, _):
                handleResultAdded(new)
            case .identical:
                break
            @unknown default:
                break
            }
        }
    }

    private func handleResultAdded(_ result: NWBrowser.Result) {
        guard case .service(let name, _, _, _) = result.endpoint else { return }

        // Skip our own published service
        let ownPrefix = "MixBridge-\(UIDevice.current.name.replacingOccurrences(of: " ", with: "-"))"
        if name.hasPrefix("MixBridge-") && name.contains(ownPrefix.prefix(min(ownPrefix.count, 20))) {
            return
        }

        resolveEndpointDirectly(result.endpoint, name: name)
    }

    private func handleResultRemoved(_ result: NWBrowser.Result) {
        guard case .service(let name, _, _, _) = result.endpoint else { return }

        lock.lock()
        knownSources.removeValue(forKey: name)
        lock.unlock()

        DispatchQueue.main.async { [weak self] in
            self?.onSourceLost?(name)
        }
    }

    /// Resolve Bonjour service to IP + port.
    private func resolveEndpointDirectly(_ endpoint: NWEndpoint, name: String) {
        let params = NWParameters.udp
        params.includePeerToPeer = true
        if let ipOptions = params.defaultProtocolStack.internetProtocol as? NWProtocolIP.Options {
            ipOptions.version = .v4
        }

        let connection = NWConnection(to: endpoint, using: params)

        let queue = DispatchQueue(label: "MixBridge.DirectResolve.\(name)",
                                   qos: .userInitiated)

        connection.stateUpdateHandler = { [weak self] state in
            guard let self else { return }

            if case .ready = state {
                if let remote = connection.currentPath?.remoteEndpoint,
                   case .hostPort(let host, let port) = remote {
                    let hostStr = MixBridgeNetworkHelpers.cleanHostString(host.debugDescription)
                    guard hostStr.contains(".") else {
                        connection.cancel()
                        return
                    }
                    let source  = DiscoveredSource(
                        name:         name,
                        address:      hostStr,
                        port:         port.rawValue,
                        computerName: name)

                    self.lock.lock()
                    let isNew = self.knownSources[name] == nil
                    self.knownSources[name] = source
                    self.lock.unlock()

                    if isNew {
                        DispatchQueue.main.async {
                            self.onSourceFound?(source)
                        }
                    }
                }
                connection.cancel()
            } else if case .failed(let err) = state {
                connection.cancel()
            } else if case .cancelled = state {
                // Done
            }
        }

        connection.start(queue: queue)

        queue.asyncAfter(deadline: .now() + 5) {
            connection.cancel()
        }
    }

    // MARK: - Fallback UDP Broadcast Discovery

    private func startFallbackBroadcastListener() {
        guard fallbackSocket < 0 else { return }

        fallbackSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fallbackSocket >= 0 else { return }

        var reuse: Int32 = 1
        setsockopt(fallbackSocket, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   socklen_t(MemoryLayout<Int32>.size))
        setsockopt(fallbackSocket, SOL_SOCKET, SO_REUSEPORT, &reuse,
                   socklen_t(MemoryLayout<Int32>.size))

        var bc: Int32 = 1
        setsockopt(fallbackSocket, SOL_SOCKET, SO_BROADCAST, &bc,
                   socklen_t(MemoryLayout<Int32>.size))

        var addr = sockaddr_in()
        addr.sin_family      = sa_family_t(AF_INET)
        addr.sin_port        = UInt16(51235).bigEndian
        addr.sin_addr.s_addr = INADDR_ANY

        withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fallbackSocket, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }

        var tv = timeval(tv_sec: 0, tv_usec: 200_000)
        setsockopt(fallbackSocket, SOL_SOCKET, SO_RCVTIMEO, &tv,
                   socklen_t(MemoryLayout<timeval>.size))

        let thread = Thread { [weak self] in
            self?.fallbackLoop()
        }
        thread.name = "MixBridge.FallbackDiscovery"
        thread.qualityOfService = .utility
        thread.start()
        fallbackThread = thread
    }

    private func fallbackLoop() {
        let kDiscoveryRequest: [UInt8] = [0x4D, 0x58, 0x42, 0x52, 0x44, 0x3F, 0, 0]

        var broadcastAddr = sockaddr_in()
        broadcastAddr.sin_family      = sa_family_t(AF_INET)
        broadcastAddr.sin_port        = UInt16(51235).bigEndian
        broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST

        var recvBuf = [UInt8](repeating: 0, count: 256)
        var senderAddr = sockaddr_in()
        var senderLen  = socklen_t(MemoryLayout<sockaddr_in>.size)

        var lastBroadcastMs: UInt64 = 0

        while isBrowsing && fallbackSocket >= 0 {
            let nowMs = HighResClock.nowMs()
            if nowMs - lastBroadcastMs >= 2000 {
                kDiscoveryRequest.withUnsafeBytes { ptr in
                    withUnsafePointer(to: &broadcastAddr) {
                        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { destPtr in
                            sendto(fallbackSocket, ptr.baseAddress, ptr.count, 0,
                                   destPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
                        }
                    }
                }
                lastBroadcastMs = nowMs
            }

            let received = withUnsafeMutablePointer(to: &senderAddr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { addrPtr in
                    recvfrom(fallbackSocket, &recvBuf, recvBuf.count,
                             0, addrPtr, &senderLen)
                }
            }

            guard received >= 8 else { continue }

            if recvBuf[0] == 0x4D && recvBuf[1] == 0x58 && recvBuf[2] == 0x42 &&
               recvBuf[3] == 0x52 && recvBuf[4] == 0x44 && recvBuf[5] == 0x21 {

                let pluginPort = UInt16(recvBuf[6]) << 8 | UInt16(recvBuf[7])

                var ipBuf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                inet_ntop(AF_INET, &senderAddr.sin_addr, &ipBuf,
                          socklen_t(INET_ADDRSTRLEN))
                let ip = String(cString: ipBuf)

                let name = "MixBridge-\(ip)"

                lock.lock()
                let alreadyKnown = knownSources[name] != nil
                if !alreadyKnown {
                    let source = DiscoveredSource(
                        name:         name,
                        address:      ip,
                        port:         pluginPort,
                        computerName: name)
                    knownSources[name] = source
                    lock.unlock()
                    DispatchQueue.main.async { [weak self] in
                        self?.onSourceFound?(source)
                    }
                } else {
                    lock.unlock()
                }
            }
        }
    }

    private func stopFallbackListener() {
        if fallbackSocket >= 0 {
            close(fallbackSocket)
            fallbackSocket = -1
        }
    }

    // MARK: - NetServiceDelegate

    func netServiceDidPublish(_ sender: NetService) {
    }

    func netService(_ sender: NetService, didNotPublish errorDict: [String: NSNumber]) {
    }
}
