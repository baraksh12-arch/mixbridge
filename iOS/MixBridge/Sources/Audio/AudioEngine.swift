// AudioEngine.swift
// Manages AVAudioEngine for MixBridge receiver.
//
// Uses AVAudioSourceNode for pull-based, gapless rendering from the jitter buffer.

import AVFoundation
import Combine

// MARK: - AudioEngineState

enum AudioEngineState {
    case idle
    case starting
    case running
    case interrupted
    case error(Error)
}

// MARK: - AudioEngine

final class AudioEngine: ObservableObject {

    // MARK: Published

    @Published private(set) var state: AudioEngineState = .idle
    @Published private(set) var outputSampleRate: Double = 48000
    @Published private(set) var outputLatencyMs: Double = 0
    @Published private(set) var signalPeakL: Float = 0
    @Published private(set) var signalPeakR: Float = 0

    // MARK: Private

    private let engine         = AVAudioEngine()
    private let jitterBuffer: JitterBuffer
    private var sourceNode: AVAudioSourceNode?

    private var streamFormat: AVAudioFormat?
    private var renderFormat: AVAudioFormat?

    private var holdPeakL: Float = 0
    private var holdPeakR: Float = 0
    private var holdCountL: Int  = 0
    private var holdCountR: Int  = 0

    private let peakHoldFrames = 44100
    private let peakDecay: Float = 0.9999

    private var sessionSampleRate: Double = 48000
    private var sessionFramesPerPacket: Int = 256

    private var observers: [NSObjectProtocol] = []

    // MARK: - Init

    init(jitterBuffer: JitterBuffer) {
        self.jitterBuffer = jitterBuffer
    }

    deinit {
        stop()
        observers.forEach { NotificationCenter.default.removeObserver($0) }
    }

    // MARK: - Public API

    func configure(sampleRate: Double, framesPerPacket: Int) throws {
        let newRate   = max(sampleRate, 44100.0)
        let newFrames = max(framesPerPacket, 64)

        let sameParams = abs(sessionSampleRate - newRate) < 1.0
            && sessionFramesPerPacket == newFrames
            && sourceNode != nil

        sessionSampleRate      = newRate
        sessionFramesPerPacket = newFrames
        jitterBuffer.configure(sampleRate: sessionSampleRate,
                               framesPerPacket: sessionFramesPerPacket)

        if sameParams, case .running = state {
            return
        }

        if case .running = state { stop() }

        do {
            try start()
        } catch {
            throw error
        }
    }

    func start() throws {
        try configureAudioSession()
        try buildEngineGraph()
        try engine.start()

        DispatchQueue.main.async {
            self.state            = .running
            self.outputSampleRate = self.engine.outputNode.outputFormat(forBus: 0).sampleRate
            self.outputLatencyMs  = Double(AVAudioSession.sharedInstance().outputLatency) * 1000
        }

        registerForNotifications()
    }

    func stop() {
        if let node = sourceNode {
            engine.detach(node)
            sourceNode = nil
        }
        engine.stop()

        DispatchQueue.main.async {
            self.state = .idle
        }
    }

    // MARK: - Audio Session

    private func configureAudioSession() throws {
        let session = AVAudioSession.sharedInstance()

        try session.setCategory(
            .playback,
            mode: .default,
            options: [.mixWithOthers]
        )

        try session.setPreferredSampleRate(sessionSampleRate)

        // ~10ms hardware buffer for smooth output
        try session.setPreferredIOBufferDuration(0.010)

        try session.setActive(true)
    }

    // MARK: - Engine Graph

    private func buildEngineGraph() throws {
        engine.reset()

        let hwFormat = engine.outputNode.outputFormat(forBus: 0)

        streamFormat = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: sessionSampleRate,
            channels: 2,
            interleaved: false
        )

        renderFormat = hwFormat

        guard let streamFmt = streamFormat,
              let renderFmt = renderFormat else {
            throw AudioEngineError.formatNegotiationFailed
        }

        let jitter = jitterBuffer
        let renderBlock: AVAudioSourceNodeRenderBlock = { _, _, frameCount, audioBufferList -> OSStatus in
            let abl = UnsafeMutableAudioBufferListPointer(audioBufferList)
            guard abl.count >= 2,
                  let lPtr = abl[0].mData?.assumingMemoryBound(to: Float.self),
                  let rPtr = abl[1].mData?.assumingMemoryBound(to: Float.self) else {
                return noErr
            }
            _ = jitter.pull(left: lPtr, right: rPtr, frameCount: Int(frameCount))
            return noErr
        }

        let node = AVAudioSourceNode(format: streamFmt, renderBlock: renderBlock)
        sourceNode = node

        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: streamFmt)
        engine.connect(engine.mainMixerNode, to: engine.outputNode, format: renderFmt)

        installMeterTap()

        try engine.prepare()
    }

    // MARK: - Meter Tap

    private func installMeterTap() {
        let mixerNode = engine.mainMixerNode
        let tapFormat = mixerNode.outputFormat(forBus: 0)
        let bufferSize = AVAudioFrameCount(tapFormat.sampleRate / 60)

        mixerNode.removeTap(onBus: 0)
        mixerNode.installTap(onBus: 0, bufferSize: bufferSize, format: tapFormat) {
            [weak self] buffer, _ in
            self?.processMeterTap(buffer: buffer)
        }
    }

    private func processMeterTap(buffer: AVAudioPCMBuffer) {
        guard let channelData = buffer.floatChannelData else { return }
        let frameCount = Int(buffer.frameLength)
        guard frameCount > 0 else { return }

        var peakL: Float = 0
        var peakR: Float = 0

        let leftData  = channelData[0]
        let rightData = buffer.format.channelCount > 1 ? channelData[1] : channelData[0]

        for i in 0..<frameCount {
            let absL = abs(leftData[i])
            let absR = abs(rightData[i])
            if absL > peakL { peakL = absL }
            if absR > peakR { peakR = absR }
        }

        if peakL >= holdPeakL {
            holdPeakL  = peakL
            holdCountL = peakHoldFrames
        } else {
            if holdCountL > 0 { holdCountL -= frameCount }
            else              { holdPeakL  *= peakDecay  }
        }

        if peakR >= holdPeakR {
            holdPeakR  = peakR
            holdCountR = peakHoldFrames
        } else {
            if holdCountR > 0 { holdCountR -= frameCount }
            else              { holdPeakR  *= peakDecay  }
        }

        let snapL = holdPeakL
        let snapR = holdPeakR

        DispatchQueue.main.async { [weak self] in
            self?.signalPeakL = snapL
            self?.signalPeakR = snapR
        }
    }

    // MARK: - Notification Handling

    private func registerForNotifications() {
        let nc = NotificationCenter.default

        let interruptionObs = nc.addObserver(
            forName: AVAudioSession.interruptionNotification,
            object: nil, queue: .main) { [weak self] note in
                self?.handleInterruption(note)
        }

        let routeChangeObs = nc.addObserver(
            forName: AVAudioSession.routeChangeNotification,
            object: nil, queue: .main) { [weak self] note in
                self?.handleRouteChange(note)
        }

        let mediaResetObs = nc.addObserver(
            forName: AVAudioSession.mediaServicesWereResetNotification,
            object: nil, queue: .main) { [weak self] _ in
                self?.handleMediaServicesReset()
        }

        observers = [interruptionObs, routeChangeObs, mediaResetObs]
    }

    private func handleInterruption(_ notification: Notification) {
        guard let info = notification.userInfo,
              let typeValue = info[AVAudioSessionInterruptionTypeKey] as? UInt,
              let type = AVAudioSession.InterruptionType(rawValue: typeValue)
        else { return }

        switch type {
        case .began:
            engine.pause()
            state = .interrupted

        case .ended:
            guard let optionsValue = info[AVAudioSessionInterruptionOptionKey] as? UInt else {
                return
            }
            let options = AVAudioSession.InterruptionOptions(rawValue: optionsValue)
            if options.contains(.shouldResume) {
                do {
                    try AVAudioSession.sharedInstance().setActive(true)
                    try engine.start()
                    state = .running
                } catch {
                    state = .error(error)
                }
            }

        @unknown default:
            break
        }
    }

    private func handleRouteChange(_ notification: Notification) {
        guard let info = notification.userInfo,
              let reasonValue = info[AVAudioSessionRouteChangeReasonKey] as? UInt,
              let reason = AVAudioSession.RouteChangeReason(rawValue: reasonValue)
        else { return }

        switch reason {
        case .oldDeviceUnavailable:
            break
        case .newDeviceAvailable, .categoryChange:
            do {
                try buildEngineGraph()
                try engine.start()
                outputLatencyMs = Double(AVAudioSession.sharedInstance().outputLatency) * 1000
            } catch {
                state = .error(error)
            }
        default:
            break
        }
    }

    private func handleMediaServicesReset() {
        do {
            try start()
        } catch {
            state = .error(error)
        }
    }
}

// MARK: - Errors

enum AudioEngineError: LocalizedError {
    case formatNegotiationFailed
    case engineStartFailed

    var errorDescription: String? {
        switch self {
        case .formatNegotiationFailed: return "Failed to negotiate audio format with plugin"
        case .engineStartFailed:       return "Failed to start audio engine"
        }
    }
}
