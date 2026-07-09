// JitterBuffer.swift
// Adaptive jitter buffer for MixBridge receiver.
//
// Accepts out-of-order UDP audio packets, sorts by sequence number,
// and delivers audio to AVAudioEngine at the correct time.

import Foundation
import AVFoundation

// MARK: - Buffered Audio Packet

struct BufferedPacket {
    let sequenceNumber: UInt32
    let captureTimestampNs: UInt64
    let sampleOffset: UInt64
    let sampleRate: UInt32
    var frameCount: UInt32
    var samples: [Float]        // Interleaved L,R,L,R,...
    var sampleReadOffset: Int = 0  // Frames already consumed from front

    var durationMs: Double {
        guard sampleRate > 0 else { return 0 }
        return Double(frameCount) / Double(sampleRate) * 1000.0
    }
}

// MARK: - JitterBuffer

final class JitterBuffer {

    // MARK: Configuration

    enum Mode {
        case auto
        case ultraLow
        case low
        case normal
        case stable

        var targetMs: Double {
            switch self {
            case .auto:     return 0
            case .ultraLow: return 2
            case .low:      return 5
            case .normal:   return 10
            case .stable:   return 60
            }
        }
    }

    // MARK: Properties

    var mode: Mode = .auto {
        didSet { updateTargetDepth() }
    }

    private(set) var targetDepthMs: Double = 60.0
    private(set) var currentDepthMs: Double = 0.0
    private(set) var isReadyForPlayback: Bool = false
    private(set) var totalPacketsReceived: UInt64 = 0
    private(set) var totalPacketsDropped: UInt64 = 0
    private(set) var totalGapsInserted: UInt64 = 0
    private(set) var totalGapsSkipped: UInt64 = 0

    // MARK: Private State

    private var packets: [UInt32: BufferedPacket] = [:]
    private let lock = NSLock()

    private var nextExpectedSeq: UInt32 = 0
    private var initialized: Bool = false

    private var jitterHistory: [Double] = []
    private var lastArrivalTimeNs: UInt64 = 0
    private var adaptiveTargetMs: Double = 60.0

    private var sampleRate: Double = 48000
    private var framesPerPacket: Int = 256
    private var playbackStarted: Bool = false

    private var lastSampleL: Float = 0
    private var lastSampleR: Float = 0

    // MARK: - Public API

    func configure(sampleRate: Double, framesPerPacket: Int) {
        self.sampleRate      = sampleRate
        self.framesPerPacket = framesPerPacket
        updateTargetDepth()
    }

    func push(_ packet: BufferedPacket) {
        lock.lock()
        defer { lock.unlock() }

        totalPacketsReceived += 1

        if !initialized {
            nextExpectedSeq = packet.sequenceNumber
            initialized     = true
        }

        let nowNs = HighResClock.nowNs()
        if lastArrivalTimeNs > 0 {
            let interarrivalMs = Double(nowNs - lastArrivalTimeNs) / 1_000_000.0
            recordJitter(interarrivalMs)
        }
        lastArrivalTimeNs = nowNs

        if packet.sequenceNumber < nextExpectedSeq {
            totalPacketsDropped += 1
            return
        }

        if Int64(packet.sequenceNumber) - Int64(nextExpectedSeq) > 500 {
            reset(to: packet.sequenceNumber)
        }

        packets[packet.sequenceNumber] = packet

        let maxPackets = Int(sampleRate / Double(framesPerPacket) * 3.0)
        while packets.count > maxPackets {
            if let minKey = packets.keys.min() {
                packets.removeValue(forKey: minKey)
                totalPacketsDropped += 1
            }
        }

        updateDepthMetrics()
    }

    @discardableResult
    func pull(into buffer: AVAudioPCMBuffer) -> Bool {
        guard let left  = buffer.floatChannelData?[0],
              let right = buffer.floatChannelData?[1] else { return false }
        return pull(left: left, right: right, frameCount: Int(buffer.frameCapacity))
    }

    /// Pull PCM directly into channel buffers (realtime-safe: no allocations).
    @discardableResult
    func pull(left: UnsafeMutablePointer<Float>,
              right: UnsafeMutablePointer<Float>,
              frameCount: Int) -> Bool {
        lock.lock()
        defer { lock.unlock() }

        guard initialized else {
            holdFill(left: left, right: right, frameCount: frameCount)
            return false
        }

        updateDepthMetrics()

        if !playbackStarted {
            isReadyForPlayback = currentDepthMs >= targetDepthMs * 0.85
            if !isReadyForPlayback {
                holdFill(left: left, right: right, frameCount: frameCount)
                return false
            }
            playbackStarted = true
        }

        var framesWritten = 0
        var hadRealAudio  = false

        while framesWritten < frameCount {
            if packets[nextExpectedSeq] == nil {
                // Skip small gaps when later packets already arrived
                var skipTo: UInt32?
                for ahead in 1...5 {
                    let candidate = nextExpectedSeq &+ UInt32(ahead)
                    if packets[candidate] != nil {
                        skipTo = candidate
                        break
                    }
                }
                if let skipTo {
                    totalGapsSkipped += UInt64(skipTo - nextExpectedSeq)
                    nextExpectedSeq = skipTo
                    continue
                }

                // No packet yet — hold last sample (no silence clicks)
                left[framesWritten]  = lastSampleL
                right[framesWritten] = lastSampleR
                framesWritten += 1
                totalGapsInserted += 1
                continue
            }

            guard var packet = packets[nextExpectedSeq] else { continue }

            let remaining    = frameCount - framesWritten
            let packetFrames = Int(packet.frameCount)
            let toCopy       = min(remaining, packetFrames)
            let baseSample   = packet.sampleReadOffset * 2

            for i in 0..<toCopy {
                let srcIdx = baseSample + i * 2
                if srcIdx + 1 < packet.samples.count {
                    let sL = packet.samples[srcIdx]
                    let sR = packet.samples[srcIdx + 1]
                    left[framesWritten + i]  = sL
                    right[framesWritten + i] = sR
                    lastSampleL = sL
                    lastSampleR = sR
                    hadRealAudio = true
                } else {
                    left[framesWritten + i]  = lastSampleL
                    right[framesWritten + i] = lastSampleR
                }
            }

            framesWritten += toCopy

            if toCopy < packetFrames {
                packet.sampleReadOffset += toCopy
                packet.frameCount = UInt32(packetFrames - toCopy)
                packets[nextExpectedSeq] = packet
            } else {
                packets.removeValue(forKey: nextExpectedSeq)
                nextExpectedSeq &+= 1
            }
        }

        updateDepthMetrics()
        return hadRealAudio
    }

    func reset(to startSeq: UInt32 = 0) {
        lock.lock()
        defer { lock.unlock() }
        packets.removeAll(keepingCapacity: true)
        nextExpectedSeq    = startSeq
        initialized        = startSeq != 0
        currentDepthMs     = 0
        isReadyForPlayback = false
        playbackStarted    = false
        lastArrivalTimeNs  = 0
        lastSampleL        = 0
        lastSampleR        = 0
    }

    // MARK: - Private

    private func holdFill(left: UnsafeMutablePointer<Float>,
                          right: UnsafeMutablePointer<Float>,
                          frameCount: Int) {
        for i in 0..<frameCount {
            left[i]  = lastSampleL
            right[i] = lastSampleR
        }
    }

    private func updateDepthMetrics() {
        if packets.isEmpty {
            currentDepthMs = 0
            return
        }
        let totalFrames = packets.values.reduce(0) { $0 + Int($1.frameCount) }
        currentDepthMs = Double(totalFrames) / sampleRate * 1000.0
    }

    private func recordJitter(_ interarrivalMs: Double) {
        jitterHistory.append(interarrivalMs)
        if jitterHistory.count > 100 {
            jitterHistory.removeFirst()
        }
        if mode == .auto {
            updateAdaptiveTarget()
        }
    }

    private func updateAdaptiveTarget() {
        guard jitterHistory.count >= 10 else { return }

        let mean = jitterHistory.reduce(0, +) / Double(jitterHistory.count)
        let variance = jitterHistory.map { pow($0 - mean, 2) }.reduce(0, +)
                       / Double(jitterHistory.count)
        let stdDev = variance.squareRoot()

        let packetIntervalMs = Double(framesPerPacket) / sampleRate * 1000.0
        adaptiveTargetMs = max(30.0, packetIntervalMs + stdDev * 4.0)
        adaptiveTargetMs = min(adaptiveTargetMs, 120.0)
        targetDepthMs    = adaptiveTargetMs
    }

    private func updateTargetDepth() {
        switch mode {
        case .auto:
            targetDepthMs = adaptiveTargetMs > 0 ? adaptiveTargetMs : 60.0
        case .ultraLow, .low, .normal, .stable:
            targetDepthMs = mode.targetMs
        }
    }
}

// MARK: - HighResClock (Swift)

enum HighResClock {
    static func nowNs() -> UInt64 {
        var ts = timespec()
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts)
        return UInt64(ts.tv_sec) * 1_000_000_000 + UInt64(ts.tv_nsec)
    }

    static func nowMs() -> UInt64 {
        return nowNs() / 1_000_000
    }
}
