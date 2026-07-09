// UDPReceiver.swift
// Low-level UDP socket receiver for MixBridge iOS.
// Runs on a dedicated high-priority thread.
// Parses incoming MixBridge protocol packets and dispatches to handlers.

import Foundation
import Network
import UIKit

// MARK: - Packet Dispatch Protocol

protocol UDPReceiverDelegate: AnyObject {
    func didReceiveAudioFrame(sequenceNumber: UInt32,
                               captureTimestampNs: UInt64,
                               sampleRate: UInt32,
                               frameCount: UInt32,
                               sampleOffset: UInt64,
                               peakL: Float, peakR: Float,
                               samples: [Float],
                               payloadCRC: UInt32)

    func didReceiveHello(senderTimestampNs: UInt64,
                          pluginName: String,
                          computerName: String,
                          fromEndpoint: NWEndpoint)

    func didReceiveSessionConfig(sessionId: UInt32,
                                  sampleRate: UInt32,
                                  blockSize: UInt16,
                                  framesPerPacket: UInt32,
                                  bitrateKbps: UInt32,
                                  latencyMode: UInt8)

    func didReceiveHeartbeat(senderTimestampNs: UInt64,
                              signalL: Float, signalR: Float,
                              fromEndpoint: NWEndpoint)

    func didReceiveClockSync(t1Ns: UInt64, fromEndpoint: NWEndpoint)

    func didReceiveGoodbye()

    func didReceiveStatsResponse(_ stats: StatsSnapshot)
}

// MARK: - Stats snapshot from plugin

struct StatsSnapshot {
    let uptimeMs: UInt64
    let totalFramesSent: UInt64
    let packetsSent: UInt32
    let packetsLost: UInt32
    let lossPercent: Float
    let latencyMs: Float
    let jitterMs: Float
    let peakL: Float
    let peakR: Float
    let sampleRate: UInt32
    let bufferSize: UInt32
    let networkQuality: UInt8
    let latencyMode: UInt8
}

// MARK: - UDPReceiver

final class UDPReceiver {

    // MARK: Constants

    private let kMagicHeader: UInt32  = 0x4D585247  // "MXRG"
    private let kProtocolVersion: UInt8 = 1
    private let kMaxPacketSize = 65507
    private let kDefaultPort: UInt16 = 51234

    // MARK: Properties

    weak var delegate: UDPReceiverDelegate?

    private(set) var isRunning: Bool = false
    private(set) var boundPort: UInt16 = 0

    // MARK: Private

    // We use POSIX sockets directly for lowest latency
    // (NWConnection has too much overhead for high-frequency audio packets)
    private var socketFD: Int32 = -1
    private var receiveThread: Thread?
    private var shouldStop = false

    // MARK: - Public API

    func start(port: UInt16 = 0) throws {
        guard !isRunning else { return }

        socketFD = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard socketFD >= 0 else {
            throw UDPError.socketCreationFailed(errno)
        }

        // Socket options
        var reuse: Int32 = 1
        setsockopt(socketFD, SOL_SOCKET, SO_REUSEADDR, &reuse, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(socketFD, SOL_SOCKET, SO_REUSEPORT, &reuse, socklen_t(MemoryLayout<Int32>.size))

        // Large receive buffer (2MB)
        var rcvBuf: Int32 = 2 * 1024 * 1024
        setsockopt(socketFD, SOL_SOCKET, SO_RCVBUF, &rcvBuf, socklen_t(MemoryLayout<Int32>.size))

        // Bind
        var addr = sockaddr_in()
        addr.sin_family      = sa_family_t(AF_INET)
        addr.sin_port        = (port == 0 ? kDefaultPort : port).bigEndian
        addr.sin_addr.s_addr = INADDR_ANY

        let bindResult = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(socketFD, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }

        if bindResult != 0 {
            // Try any port if preferred port taken
            addr.sin_port = 0
            let fallbackResult = withUnsafePointer(to: &addr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    bind(socketFD, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
            if fallbackResult != 0 {
                close(socketFD)
                socketFD = -1
                throw UDPError.bindFailed(errno)
            }
        }

        // Get actual bound port
        var boundAddr = sockaddr_in()
        var addrLen = socklen_t(MemoryLayout<sockaddr_in>.size)
        withUnsafeMutablePointer(to: &boundAddr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                getsockname(socketFD, $0, &addrLen)
            }
        }
        boundPort = boundAddr.sin_port.bigEndian

        // Set 5ms receive timeout (so thread can check shouldStop)
        var timeout = timeval(tv_sec: 0, tv_usec: 5000)
        setsockopt(socketFD, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   socklen_t(MemoryLayout<timeval>.size))

        isRunning  = true
        shouldStop = false

        let thread = Thread {
            // Boost thread priority for lowest latency
            Thread.current.threadPriority = 1.0
            self.receiveLoop()
        }
        thread.name = "MixBridge.UDPReceive"
        thread.qualityOfService = .userInteractive
        thread.start()
        receiveThread = thread
    }

    func stop() {
        shouldStop = true
        isRunning  = false
        if socketFD >= 0 {
            close(socketFD)
            socketFD = -1
        }
    }

    // MARK: - Send (for control replies)

    func send(_ data: Data, to address: String, port: UInt16) {
        guard socketFD >= 0 else { return }

        var dest = sockaddr_in()
        dest.sin_family = sa_family_t(AF_INET)
        dest.sin_port   = port.bigEndian
        guard inet_pton(AF_INET, address, &dest.sin_addr) == 1 else {
            return
        }

        let sent = data.withUnsafeBytes { ptr -> Int in
            withUnsafePointer(to: &dest) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { destPtr in
                    sendto(socketFD, ptr.baseAddress, data.count, 0,
                           destPtr, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
    }

    // MARK: - Receive Loop

    private func receiveLoop() {
        let bufSize = kMaxPacketSize
        let buf     = UnsafeMutablePointer<UInt8>.allocate(capacity: bufSize)
        defer { buf.deallocate() }

        var senderAddr = sockaddr_in()
        var senderLen  = socklen_t(MemoryLayout<sockaddr_in>.size)

        while !shouldStop {
            guard socketFD >= 0 else { break }

            let received = withUnsafeMutablePointer(to: &senderAddr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { addrPtr in
                    recvfrom(socketFD, buf, bufSize, 0, addrPtr, &senderLen)
                }
            }

            if received <= 0 {
                // Timeout or error - loop and check shouldStop
                continue
            }

            let senderIP: String = {
                var ipBuf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                inet_ntop(AF_INET, &senderAddr.sin_addr, &ipBuf, socklen_t(INET_ADDRSTRLEN))
                return String(cString: ipBuf)
            }()
            let senderPort = senderAddr.sin_port.bigEndian

            parsePacket(buf, size: received,
                        senderIP: senderIP, senderPort: senderPort)
        }
    }

    // MARK: - Packet Parser

    private func parsePacket(_ buf: UnsafePointer<UInt8>, size: Int,
                              senderIP: String, senderPort: UInt16) {
        guard size >= 16 else { return }

        // Read magic
        let magic = buf.withMemoryRebound(to: UInt32.self, capacity: 1) { $0.pointee }
        guard magic == kMagicHeader else { return }

        let version = buf[4]
        guard version == kProtocolVersion else { return }

        let packetType = buf[5]
        // Sequence number at offset 8
        let seqNumber  = readUInt32(buf, offset: 8)

        // Validate header CRC
        let crcValid = validateHeaderCRC(buf)
        if !crcValid {
            return
        }

        let endpoint = NWEndpoint.hostPort(
            host: NWEndpoint.Host(senderIP),
            port: NWEndpoint.Port(rawValue: senderPort)!)

        switch packetType {
        case 0x01: parseHello(buf, size: size, from: endpoint)
        case 0x03: parseGoodbye()
        case 0x04: parseHeartbeat(buf, size: size, from: endpoint)
        case 0x06: parseSessionConfig(buf, size: size)
        case 0x10: parseAudioFrame(buf, size: size)
        case 0x20: parseClockSync(buf, size: size, from: endpoint)
        case 0x31: parseStatsResponse(buf, size: size)
        default:   break
        }
    }

    // MARK: - Individual Parsers

    private func parseHello(_ buf: UnsafePointer<UInt8>, size: Int,
                             from endpoint: NWEndpoint) {
        guard size >= ProtocolPacketSize.hello else { return }
        // Layout: header(16) + timestamp(8) + pluginName(32) + computerName(64) + caps(1) + pad(3)
        let timestamp  = readUInt64(buf, offset: 16)
        let pluginName = readString(buf, offset: 24, maxLength: 32)
        let compName   = readString(buf, offset: 56, maxLength: 64)

        delegate?.didReceiveHello(senderTimestampNs: timestamp,
                                   pluginName: pluginName,
                                   computerName: compName,
                                   fromEndpoint: endpoint)
    }

    private func parseGoodbye() {
        delegate?.didReceiveGoodbye()
    }

    private func parseHeartbeat(_ buf: UnsafePointer<UInt8>, size: Int,
                                 from endpoint: NWEndpoint) {
        guard size >= ProtocolPacketSize.heartbeat else { return }
        let timestamp = readUInt64(buf, offset: 16)
        let sigL      = readFloat(buf, offset: 32)
        let sigR      = readFloat(buf, offset: 36)

        delegate?.didReceiveHeartbeat(senderTimestampNs: timestamp,
                                       signalL: sigL, signalR: sigR,
                                       fromEndpoint: endpoint)
    }

    private func parseSessionConfig(_ buf: UnsafePointer<UInt8>, size: Int) {
        guard size >= ProtocolPacketSize.sessionConfig else { return }
        let sessionId       = readUInt32(buf, offset: 12)
        let sampleRate      = readUInt32(buf, offset: 16)
        let blockSize       = readUInt16(buf, offset: 20)
        let framesPerPacket = readUInt32(buf, offset: 24)
        let bitrate         = readUInt32(buf, offset: 28)
        let latencyMode     = buf[32]

        delegate?.didReceiveSessionConfig(sessionId: sessionId,
                                           sampleRate: sampleRate,
                                           blockSize: blockSize,
                                           framesPerPacket: framesPerPacket,
                                           bitrateKbps: bitrate,
                                           latencyMode: latencyMode)
    }

    private func parseAudioFrame(_ buf: UnsafePointer<UInt8>, size: Int) {
        // AudioFrameHeader = 60 bytes (16 header + 44 payload header)
        guard size >= ProtocolPacketSize.audioFrameHeader else { return }

        let seqNum      = readUInt32(buf, offset: 8)
        let captureTs   = readUInt64(buf, offset: 16)
        let sampleRate  = readUInt32(buf, offset: 24)
        let frameCount  = readUInt32(buf, offset: 28)
        let sampleOffset = readUInt64(buf, offset: 32)  // Note: overlaps - see struct layout
        let peakL       = readFloat(buf, offset: 40)
        let peakR       = readFloat(buf, offset: 44)
        let payloadCRC  = readUInt32(buf, offset: 48)

        // Audio payload starts at offset 60
        let payloadOffset = 60
        let payloadBytes  = size - payloadOffset

        guard payloadBytes > 0 else { return }

        // Verify CRC
        let computedCRC = crc32(buf + payloadOffset, length: payloadBytes)
        guard computedCRC == payloadCRC else {
            // CRC mismatch - drop corrupted packet
            return
        }

        // Parse float samples
        let floatCount = payloadBytes / MemoryLayout<Float>.size
        var samples = [Float](repeating: 0, count: floatCount)
        (buf + payloadOffset).withMemoryRebound(to: Float.self, capacity: floatCount) {
            for i in 0..<floatCount {
                samples[i] = $0[i]
            }
        }

        delegate?.didReceiveAudioFrame(
            sequenceNumber: seqNum,
            captureTimestampNs: captureTs,
            sampleRate: sampleRate,
            frameCount: frameCount,
            sampleOffset: sampleOffset,
            peakL: peakL,
            peakR: peakR,
            samples: samples,
            payloadCRC: payloadCRC)
    }

    private func parseClockSync(_ buf: UnsafePointer<UInt8>, size: Int,
                                 from endpoint: NWEndpoint) {
        guard size >= 24 else { return }
        let t1 = readUInt64(buf, offset: 16)
        delegate?.didReceiveClockSync(t1Ns: t1, fromEndpoint: endpoint)
    }

    private func parseStatsResponse(_ buf: UnsafePointer<UInt8>, size: Int) {
        guard size >= ProtocolPacketSize.statsResponse else { return }
        let snap = StatsSnapshot(
            uptimeMs:       readUInt64(buf, offset: 16),
            totalFramesSent: readUInt64(buf, offset: 24),
            packetsSent:    readUInt32(buf, offset: 32),
            packetsLost:    readUInt32(buf, offset: 36),
            lossPercent:    readFloat(buf, offset: 40),
            latencyMs:      readFloat(buf, offset: 44),
            jitterMs:       readFloat(buf, offset: 48),
            peakL:          readFloat(buf, offset: 52),
            peakR:          readFloat(buf, offset: 56),
            sampleRate:     readUInt32(buf, offset: 60),
            bufferSize:     readUInt32(buf, offset: 64),
            networkQuality: buf[68],
            latencyMode:    buf[69])

        delegate?.didReceiveStatsResponse(snap)
    }

    // MARK: - Packet Building (for replies)

    func buildHelloAckPacket(sessionId: UInt32, seqNum: UInt32,
                              latencyMode: UInt8) -> Data {
        var data = Data(count: ProtocolPacketSize.hello)
        data.withUnsafeMutableBytes { ptr in
            guard let base = ptr.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }

            // Header
            writeUInt32(base, offset: 0, value: kMagicHeader)
            base[4] = kProtocolVersion
            base[5] = 0x02  // HelloAck
            base[6] = 0; base[7] = 0  // CRC placeholder
            writeUInt32(base, offset: 8, value: seqNum)
            writeUInt32(base, offset: 12, value: sessionId)

            // Payload
            writeUInt64(base, offset: 16, value: HighResClock.nowNs())
            writeString(base, offset: 24, value: "MixBridge iOS", maxLen: 32)
            writeString(base, offset: 56, value: UIDevice.current.name, maxLen: 64)
            base[120] = 0  // capabilities
            base[121] = latencyMode

            // Compute and write header CRC
            let crc = computeHeaderCRC(base)
            writeUInt16(base, offset: 6, value: crc)
        }
        return data
    }

    func buildSessionAckPacket(sessionId: UInt32, seqNum: UInt32,
                                accepted: Bool, latencyMode: UInt8,
                                jitterBufferMs: UInt16) -> Data {
        var data = Data(count: 24)
        data.withUnsafeMutableBytes { ptr in
            guard let base = ptr.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
            writeUInt32(base, offset: 0, value: kMagicHeader)
            base[4] = kProtocolVersion
            base[5] = 0x07  // SessionAck
            base[6] = 0; base[7] = 0
            writeUInt32(base, offset: 8, value: seqNum)
            writeUInt32(base, offset: 12, value: sessionId)
            base[16] = accepted ? 1 : 0
            base[17] = latencyMode
            writeUInt16(base, offset: 18, value: jitterBufferMs)
            let crc = computeHeaderCRC(base)
            writeUInt16(base, offset: 6, value: crc)
        }
        return data
    }

    func buildHeartbeatAckPacket(sessionId: UInt32, seqNum: UInt32,
                                  originTs: UInt64) -> Data {
        var data = Data(count: 48)
        data.withUnsafeMutableBytes { ptr in
            guard let base = ptr.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
            writeUInt32(base, offset: 0, value: kMagicHeader)
            base[4] = kProtocolVersion
            base[5] = 0x05  // HeartbeatAck
            base[6] = 0; base[7] = 0
            writeUInt32(base, offset: 8, value: seqNum)
            writeUInt32(base, offset: 12, value: sessionId)
            writeUInt64(base, offset: 16, value: originTs)
            writeUInt64(base, offset: 24, value: HighResClock.nowNs())
            writeUInt32(base, offset: 32, value: 0)  // jitter buffer ms
            base[36] = 1  // network quality = good
            let crc = computeHeaderCRC(base)
            writeUInt16(base, offset: 6, value: crc)
        }
        return data
    }

    func buildClockSyncAckPacket(sessionId: UInt32, seqNum: UInt32,
                                  t1: UInt64, t2: UInt64) -> Data {
        var data = Data(count: 40)
        data.withUnsafeMutableBytes { ptr in
            guard let base = ptr.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
            writeUInt32(base, offset: 0, value: kMagicHeader)
            base[4] = kProtocolVersion
            base[5] = 0x21  // ClockSyncAck
            base[6] = 0; base[7] = 0
            writeUInt32(base, offset: 8, value: seqNum)
            writeUInt32(base, offset: 12, value: sessionId)
            writeUInt64(base, offset: 16, value: t1)
            writeUInt64(base, offset: 24, value: t2)
            writeUInt64(base, offset: 32, value: HighResClock.nowNs())
            let crc = computeHeaderCRC(base)
            writeUInt16(base, offset: 6, value: crc)
        }
        return data
    }

    // MARK: - Binary Helpers

    private func readUInt16(_ buf: UnsafePointer<UInt8>, offset: Int) -> UInt16 {
        return UInt16(buf[offset]) | (UInt16(buf[offset + 1]) << 8)
    }

    private func readUInt32(_ buf: UnsafePointer<UInt8>, offset: Int) -> UInt32 {
        var v: UInt32 = 0
        for i in 0..<4 { v |= UInt32(buf[offset + i]) << (i * 8) }
        return v
    }

    private func readUInt64(_ buf: UnsafePointer<UInt8>, offset: Int) -> UInt64 {
        var v: UInt64 = 0
        for i in 0..<8 { v |= UInt64(buf[offset + i]) << (i * 8) }
        return v
    }

    private func readFloat(_ buf: UnsafePointer<UInt8>, offset: Int) -> Float {
        let bits = readUInt32(buf, offset: offset)
        return Float(bitPattern: bits)
    }

    private func readString(_ buf: UnsafePointer<UInt8>, offset: Int,
                             maxLength: Int) -> String {
        var bytes = [UInt8](repeating: 0, count: maxLength)
        for i in 0..<maxLength { bytes[i] = buf[offset + i] }
        return String(bytes: bytes, encoding: .utf8)?
            .trimmingCharacters(in: .init(charactersIn: "\0")) ?? ""
    }

    private func writeUInt16(_ base: UnsafeMutablePointer<UInt8>,
                              offset: Int, value: UInt16) {
        base[offset]     = UInt8(value & 0xFF)
        base[offset + 1] = UInt8(value >> 8)
    }

    private func writeUInt32(_ base: UnsafeMutablePointer<UInt8>,
                              offset: Int, value: UInt32) {
        for i in 0..<4 { base[offset + i] = UInt8((value >> (i * 8)) & 0xFF) }
    }

    private func writeUInt64(_ base: UnsafeMutablePointer<UInt8>,
                              offset: Int, value: UInt64) {
        for i in 0..<8 { base[offset + i] = UInt8((value >> (i * 8)) & 0xFF) }
    }

    private func writeString(_ base: UnsafeMutablePointer<UInt8>,
                              offset: Int, value: String, maxLen: Int) {
        let bytes = Array(value.utf8.prefix(maxLen - 1))
        for (i, b) in bytes.enumerated() { base[offset + i] = b }
    }

    private func validateHeaderCRC(_ buf: UnsafePointer<UInt8>) -> Bool {
        // Read stored CRC at bytes 6-7
        let stored = UInt16(buf[6]) | (UInt16(buf[7]) << 8)
        // Zero the CRC field and recompute
        var copy = [UInt8](repeating: 0, count: 16)
        for i in 0..<16 { copy[i] = buf[i] }
        copy[6] = 0; copy[7] = 0
        let computed = crc16(copy)
        return computed == stored
    }

    private func computeHeaderCRC(_ base: UnsafePointer<UInt8>) -> UInt16 {
        var copy = [UInt8](repeating: 0, count: 16)
        for i in 0..<16 { copy[i] = base[i] }
        copy[6] = 0; copy[7] = 0
        return crc16(copy)
    }

    private func crc16(_ data: [UInt8]) -> UInt16 {
        var crc: UInt16 = 0xFFFF
        for byte in data {
            crc ^= UInt16(byte) << 8
            for _ in 0..<8 {
                crc = (crc & 0x8000) != 0 ? (crc << 1) ^ 0x1021 : (crc << 1)
            }
        }
        return crc
    }

    private func crc32(_ data: UnsafePointer<UInt8>, length: Int) -> UInt32 {
        var crc: UInt32 = 0xFFFFFFFF
        for i in 0..<length {
            crc ^= UInt32(data[i])
            for _ in 0..<8 {
                crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320 : (crc >> 1)
            }
        }
        return crc ^ 0xFFFFFFFF
    }
}

// MARK: - Errors

enum UDPError: LocalizedError {
    case socketCreationFailed(Int32)
    case bindFailed(Int32)

    var errorDescription: String? {
        switch self {
        case .socketCreationFailed(let e): return "Socket creation failed: \(e)"
        case .bindFailed(let e):           return "Socket bind failed: \(e)"
        }
    }
}
