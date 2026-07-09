// MixBridgeNetworkHelpers.swift
// Shared network string utilities.

import Foundation

enum MixBridgeNetworkHelpers {
    /// Strip IPv6 zone ID and brackets from host strings for inet_pton / UserDefaults.
    static func cleanHostString(_ host: String) -> String {
        var s = host
        if s.hasPrefix("[") && s.hasSuffix("]") {
            s = String(s.dropFirst().dropLast())
        }
        if let pct = s.firstIndex(of: "%") {
            s = String(s[..<pct])
        }
        return s
    }
}

/// Wire-format packet sizes (must match Shared/Protocol/MixBridgeProtocol.h).
enum ProtocolPacketSize {
    static let header       = 16
    static let hello        = 124   // HelloPacket / HelloAckPacket
    static let sessionConfig = 36
    static let heartbeat    = 40
    static let audioFrameHeader = 60
    static let statsResponse = 72
}
