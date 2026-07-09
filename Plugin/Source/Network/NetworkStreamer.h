#pragma once
//==============================================================================
// NetworkStreamer.h
// Runs on a dedicated network thread.
// Pulls audio from the lock-free ring buffer and sends UDP packets.
// Handles connection lifecycle, heartbeats, and clock synchronization.
//==============================================================================

#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <chrono>
#include <array>
#include <mutex>
#include <algorithm>
#include <string>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <unistd.h>
#endif

#include "UDPSocket.h"
#include "../../Shared/Protocol/MixBridgeProtocol.h"
#include "../../Shared/RingBuffer/LockFreeRingBuffer.h"
#include "../../Shared/Common/HighResClock.h"
#include "../../Shared/Common/NetworkStats.h"
namespace MixBridge {

//==============================================================================
// Session state machine
//==============================================================================

enum class SessionState : int
{
    Idle,          // No receiver known
    Searching,     // Discovery in progress
    Connecting,    // Hello sent, waiting HelloAck
    Configuring,   // SessionConfig sent, waiting SessionAck
    Streaming,     // Active audio transmission
    Reconnecting,  // Lost connection, attempting recovery
};

//==============================================================================
// Callbacks fired on network thread (not audio thread)
//==============================================================================

struct StreamerCallbacks
{
    std::function<void(SessionState)>          onStateChanged;
    std::function<void(const EndPoint&, const std::string& deviceName)> onReceiverConnected;
    std::function<void()>                      onReceiverDisconnected;
    std::function<void(float latencyMs)>       onLatencyUpdated;
    std::function<void(const NetworkStats::Snapshot&)> onStatsUpdated;
};

//==============================================================================
// NetworkStreamer
//==============================================================================

class NetworkStreamer
{
public:
    NetworkStreamer() noexcept = default;
    ~NetworkStreamer() noexcept { stop(); }

    NetworkStreamer(const NetworkStreamer&)            = delete;
    NetworkStreamer& operator=(const NetworkStreamer&) = delete;

    //==========================================================================
    // Configure audio parameters before starting.
    //==========================================================================
    void setAudioParameters(double sampleRate, int blockSize) noexcept
    {
        sampleRate_.store(sampleRate, std::memory_order_relaxed);
        blockSize_.store(blockSize,   std::memory_order_relaxed);

        const uint32_t block = static_cast<uint32_t>(std::max(blockSize, 64));
        framesPerPacket_ = std::min(block, 256u);
    }

    //==========================================================================
    // Set signal levels for heartbeat packets (called from audio thread).
    //==========================================================================
    void updateSignalLevels(float peakL, float peakR) noexcept
    {
        signalPeakL_.store(peakL, std::memory_order_relaxed);
        signalPeakR_.store(peakR, std::memory_order_relaxed);
    }

    //==========================================================================
    // Set receiver endpoint (called from discovery).
    //==========================================================================
    void setReceiverEndpoint(const EndPoint& ep) noexcept
    {
        std::lock_guard<std::mutex> lock(endpointMutex_);
        pendingEndpoint_ = ep;
        hasNewEndpoint_.store(true, std::memory_order_release);
    }

    //==========================================================================
    // Start streaming thread.
    //==========================================================================
    bool start(AudioRingBuffer& ringBuffer, const StreamerCallbacks& callbacks)
    {
        if (running_.load()) return false;

        ringBuffer_ = &ringBuffer;
        callbacks_  = callbacks;

        if (!socket_.open(Protocol::kDefaultPort)) {
            // Try any port if default is taken
            if (!socket_.open(0)) return false;
        }
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&NetworkStreamer::runLoop, this);
        return true;
    }

    //==========================================================================
    // Stop streaming thread gracefully.
    //==========================================================================
    void stop() noexcept
    {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
        socket_.close();
    }

    //==========================================================================
    // Disconnect current receiver (sends Goodbye).
    //==========================================================================
    void disconnect() noexcept
    {
        disconnectRequested_.store(true, std::memory_order_release);
    }

    //==========================================================================
    // Force reconnect attempt.
    //==========================================================================
    void reconnect() noexcept
    {
        reconnectRequested_.store(true, std::memory_order_release);
    }

    //==========================================================================
    // Handle incoming control packet (called from receiver thread / select loop).
    //==========================================================================
    void processIncomingPacket(const uint8_t* data, size_t size, const EndPoint& sender)
    {
        if (size < sizeof(Protocol::PacketHeader)) return;

        const auto* hdr = reinterpret_cast<const Protocol::PacketHeader*>(data);
        const bool valid = hdr->isValid();
        if (!valid) return;

        const auto type = static_cast<Protocol::PacketType>(hdr->type);

        switch (type) {
            case Protocol::PacketType::HelloAck:
                handleHelloAck(data, size, sender);
                break;
            case Protocol::PacketType::SessionAck:
                handleSessionAck(data, size);
                break;
            case Protocol::PacketType::Heartbeat:
                handleHeartbeat(data, size, sender);
                break;
            case Protocol::PacketType::HeartbeatAck:
                handleHeartbeatAck(data, size);
                break;
            case Protocol::PacketType::ClockSyncAck:
                handleClockSyncAck(data, size);
                break;
            case Protocol::PacketType::Goodbye:
                handleGoodbye();
                break;
            case Protocol::PacketType::LatencyMode:
                handleLatencyMode(data, size);
                break;
            case Protocol::PacketType::StatsRequest:
                sendStatsResponse();
                break;
            default:
                break;
        }
    }

    SessionState state() const noexcept
    {
        return static_cast<SessionState>(state_.load(std::memory_order_acquire));
    }

    bool isStreaming() const noexcept
    {
        return state() == SessionState::Streaming;
    }

    const NetworkStats& stats() const noexcept { return stats_; }

    uint16_t boundPort() const noexcept { return socket_.boundPort(); }

private:
    //==========================================================================
    // Main thread loop
    //==========================================================================
    void runLoop()
    {
        socket_.setReceiveTimeout(1); // 1ms timeout for recv so loop stays responsive

        constexpr size_t kRecvBufSize = Protocol::kMaxPacketSize;
        std::array<uint8_t, kRecvBufSize> recvBuf;

        uint64_t lastHeartbeatNs     = 0;
        uint64_t lastClockSyncNs     = 0;
        uint64_t lastStatsUpdateNs   = 0;
        constexpr uint64_t kClockSyncIntervalNs = 2'000'000'000ULL; // 2 seconds
        constexpr uint64_t kStatsIntervalNs     = 1'000'000'000ULL; // 1 second

        while (running_.load(std::memory_order_acquire)) {
            const uint64_t now = HighResClock::nowNs();

            // Check for new endpoint from discovery
            if (hasNewEndpoint_.exchange(false, std::memory_order_acq_rel)) {
                std::lock_guard<std::mutex> lock(endpointMutex_);
                const SessionState epState = state();
                receiverEndpoint_ = pendingEndpoint_;
                if (epState != SessionState::Streaming &&
                    epState != SessionState::Configuring) {
                    setState(SessionState::Connecting);
                    sendHello();
                    connectAttemptNs_ = now;
                }
            }

            // Handle disconnect request
            if (disconnectRequested_.exchange(false, std::memory_order_acq_rel)) {
                sendGoodbye();
                setState(SessionState::Idle);
                receiverEndpoint_ = {};
                clockSync_.reset();
                stats_.reset();
                sequenceNumber_ = 0;
            }

            // Handle reconnect request
            if (reconnectRequested_.exchange(false, std::memory_order_acq_rel)) {
                if (receiverEndpoint_.isValid()) {
                    setState(SessionState::Reconnecting);
                    sendHello();
                    connectAttemptNs_ = now;
                }
            }

            // Receive incoming packets
            EndPoint sender;
            int received = socket_.receiveFrom(recvBuf.data(), recvBuf.size(), sender);
            if (received > 0) {
                stats_.onPacketReceived(
                    reinterpret_cast<const Protocol::PacketHeader*>(recvBuf.data())->sequenceNumber);
                processIncomingPacket(recvBuf.data(), static_cast<size_t>(received), sender);
                if (state() == SessionState::Reconnecting) {
                    setState(SessionState::Streaming);
                    lastPacketFromReceiverNs_ = now;
                }
            }

            const SessionState curState = state();

            // Connection timeout detection
            if (curState == SessionState::Connecting ||
                curState == SessionState::Configuring) {
                if (HighResClock::elapsedMs(connectAttemptNs_) > Protocol::kConnectionTimeoutMs) {
                    setState(SessionState::Reconnecting);
                    sendHello();
                    connectAttemptNs_ = now;
                }
            }

            // Lost connection detection during streaming — soft probe first
            if (curState == SessionState::Streaming) {
                const uint64_t idleMs = HighResClock::elapsedMs(lastPacketFromReceiverNs_);
                if (idleMs > Protocol::kConnectionTimeoutMs * 3) {
                    setState(SessionState::Reconnecting);
                    sendHello();
                    connectAttemptNs_ = now;
                    lastSoftProbeNs_ = now;
                    if (callbacks_.onReceiverDisconnected)
                        callbacks_.onReceiverDisconnected();
                } else if (idleMs > Protocol::kConnectionTimeoutMs &&
                           (now - lastSoftProbeNs_) >= 2'000'000'000ULL) {
                    sendHello();
                    lastSoftProbeNs_ = now;
                }
            }

            // Send audio while session is active (keep flowing during soft reconnect)
            if (curState == SessionState::Streaming ||
                curState == SessionState::Reconnecting) {
                sendPendingAudioFrames();
            }

            // Heartbeat
            if (HighResClock::elapsedMs(lastHeartbeatNs) >=
                Protocol::kHeartbeatIntervalMs && curState >= SessionState::Streaming) {
                sendHeartbeat();
                lastHeartbeatNs = now;
            }

            // Clock sync
            if (curState == SessionState::Streaming &&
                now - lastClockSyncNs >= kClockSyncIntervalNs) {
                sendClockSync();
                lastClockSyncNs = now;
            }

            // Stats update callback
            if (now - lastStatsUpdateNs >= kStatsIntervalNs) {
                if (callbacks_.onStatsUpdated)
                    callbacks_.onStatsUpdated(stats_.snapshot());
                lastStatsUpdateNs = now;
            }

            // Small sleep if nothing to do to avoid CPU spinning
            if (curState < SessionState::Streaming ||
                (ringBuffer_ && ringBuffer_->availableFrames() < 32)) {
                std::this_thread::sleep_for(std::chrono::microseconds(250));
            }
        }

        sendGoodbye();
    }

    //==========================================================================
    // Audio frame sending
    //==========================================================================
    void sendPendingAudioFrames()
    {
        if (!ringBuffer_) return;

        const uint32_t sr = static_cast<uint32_t>(
            sampleRate_.load(std::memory_order_relaxed));

        // Send in chunks of framesPerPacket_
        while (ringBuffer_->availableFrames() >= framesPerPacket_) {
            sendAudioFrame(framesPerPacket_);
        }
    }

    void sendAudioFrame(uint32_t frameCount)
    {
        // Total packet = header + interleaved float data
        const size_t payloadBytes = frameCount * 2 * sizeof(float);
        const size_t totalSize    = sizeof(Protocol::AudioFrameHeader) + payloadBytes;

        // Use local stack buffer (max ~64KB, well within stack limit for small blocks)
        // For very large blocks we'd need a heap buffer - but typical blocks are 64-512 frames
        static thread_local std::array<uint8_t, Protocol::kMaxPacketSize> packetBuf;

        if (totalSize > packetBuf.size()) return;

        auto* hdr = reinterpret_cast<Protocol::AudioFrameHeader*>(packetBuf.data());
        hdr->header.init(Protocol::PacketType::AudioFrame,
                         audioSequenceNumber_++, sessionId_);
        hdr->captureTimestampNs = HighResClock::nowNs();
        hdr->sampleRate         = static_cast<uint32_t>(
            sampleRate_.load(std::memory_order_relaxed));
        hdr->frameCount         = frameCount;
        hdr->sampleOffset       = totalFramesSent_;
        hdr->peakL              = signalPeakL_.load(std::memory_order_relaxed);
        hdr->peakR              = signalPeakR_.load(std::memory_order_relaxed);

        float* payload = reinterpret_cast<float*>(packetBuf.data() +
                         sizeof(Protocol::AudioFrameHeader));

        const size_t read = ringBuffer_->read(payload, frameCount);
        if (read != frameCount) return; // Underrun, skip this frame

        hdr->payloadCRC = Protocol::crc32(
            reinterpret_cast<const uint8_t*>(payload), payloadBytes);

        const int sent = socket_.sendTo(receiverEndpoint_, packetBuf.data(), totalSize);
        if (sent > 0) {
            stats_.onPacketSent();
            totalFramesSent_ += frameCount;
        }
    }

    //==========================================================================
    // Control packet senders
    //==========================================================================

    void sendHello()
    {
        Protocol::HelloPacket pkt{};
        pkt.header.init(Protocol::PacketType::Hello, sequenceNumber_++, sessionId_);
        pkt.senderTimestampNs = HighResClock::nowNs();

        ::strncpy(pkt.pluginName,    "MixBridge VST3",   sizeof(pkt.pluginName) - 1);
        ::strncpy(pkt.computerName,  getComputerName().c_str(),
                  sizeof(pkt.computerName) - 1);

        socket_.sendTo(receiverEndpoint_, &pkt, sizeof(pkt));
        stats_.onPacketSent();
    }

    void sendSessionConfig()
    {
        Protocol::SessionConfigPacket pkt{};
        pkt.header.init(Protocol::PacketType::SessionConfig,
                        sequenceNumber_++, sessionId_);

        const double sr = sampleRate_.load(std::memory_order_relaxed);
        const double effectiveSr = sr > 0.0 ? sr : 48000.0;
        pkt.sampleRate       = static_cast<uint32_t>(effectiveSr);
        pkt.blockSize        = static_cast<uint16_t>(
            std::max<uint32_t>(blockSize_.load(std::memory_order_relaxed), 64u));
        pkt.channelCount     = 2;
        pkt.bitsPerSample    = 32;
        pkt.framesPerPacket  = std::max(framesPerPacket_, 64u);
        pkt.nominalBitrateKbps = static_cast<uint32_t>(
            effectiveSr * 2.0 * 4.0 * 8.0 / 1000.0); // stereo * bytes * bits
        pkt.latencyMode      = static_cast<uint8_t>(latencyMode_);

        socket_.sendTo(receiverEndpoint_, &pkt, sizeof(pkt));
        stats_.onPacketSent();
    }

    void sendHeartbeat()
    {
        Protocol::HeartbeatPacket pkt{};
        pkt.header.init(Protocol::PacketType::Heartbeat,
                        sequenceNumber_++, sessionId_);
        pkt.senderTimestampNs = HighResClock::nowNs();
        pkt.packetsSent       = static_cast<uint32_t>(stats_.snapshot().packetsSent);
        pkt.packetsLost       = static_cast<uint32_t>(stats_.snapshot().packetsLost);
        pkt.signalLevelL      = signalPeakL_.load(std::memory_order_relaxed);
        pkt.signalLevelR      = signalPeakR_.load(std::memory_order_relaxed);

        socket_.sendTo(receiverEndpoint_, &pkt, sizeof(pkt));
        stats_.onPacketSent();
    }

    void sendClockSync()
    {
        Protocol::ClockSyncPacket pkt{};
        pkt.header.init(Protocol::PacketType::ClockSync,
                        sequenceNumber_++, sessionId_);
        pkt.t1Ns = clockSync_.beginSync();

        socket_.sendTo(receiverEndpoint_, &pkt, sizeof(pkt));
        stats_.onPacketSent();
    }

    void sendGoodbye()
    {
        if (!receiverEndpoint_.isValid()) return;
        Protocol::GoodbyePacket pkt{};
        pkt.header.init(Protocol::PacketType::Goodbye,
                        sequenceNumber_++, sessionId_);
        pkt.reason = 0; // Normal
        socket_.sendTo(receiverEndpoint_, &pkt, sizeof(pkt));
    }

    void sendStatsResponse()
    {
        Protocol::StatsResponsePacket pkt{};
        pkt.header.init(Protocol::PacketType::StatsResponse,
                        sequenceNumber_++, sessionId_);

        auto snap = stats_.snapshot();
        pkt.totalFramesSent   = totalFramesSent_;
        pkt.packetsSent       = static_cast<uint32_t>(snap.packetsSent);
        pkt.packetsLost       = static_cast<uint32_t>(snap.packetsLost);
        pkt.packetLossPercent = snap.lossPercent;
        pkt.currentLatencyMs  = static_cast<float>(clockSync_.rttMs() / 2.0);
        pkt.jitterMs          = static_cast<float>(snap.jitterMs);
        pkt.signalPeakL       = signalPeakL_.load(std::memory_order_relaxed);
        pkt.signalPeakR       = signalPeakR_.load(std::memory_order_relaxed);
        pkt.sampleRate        = static_cast<uint32_t>(
            sampleRate_.load(std::memory_order_relaxed));
        pkt.bufferSize        = static_cast<uint32_t>(
            blockSize_.load(std::memory_order_relaxed));
        pkt.networkQuality    = static_cast<uint8_t>(snap.quality);
        pkt.latencyMode       = static_cast<uint8_t>(latencyMode_);

        socket_.sendTo(receiverEndpoint_, &pkt, sizeof(pkt));
    }

    //==========================================================================
    // Incoming packet handlers
    //==========================================================================

    void handleHelloAck(const uint8_t* data, size_t size, const EndPoint& sender)
    {
        if (size < sizeof(Protocol::HelloAckPacket)) {
            return;
        }

        const auto* pkt = reinterpret_cast<const Protocol::HelloAckPacket*>(data);
        const auto curState = state();
        lastPacketFromReceiverNs_ = HighResClock::nowNs();

        if (curState == SessionState::Configuring) {
            return;
        }

        if (curState == SessionState::Streaming ||
            curState == SessionState::Reconnecting) {
            if (pkt->header.sessionId == sessionId_) {
                return;
            }
            // Receiver requested a new session (reconnect)
            receiverEndpoint_ = sender;
            sessionId_        = pkt->header.sessionId;
            setState(SessionState::Configuring);
            sendSessionConfig();
            connectAttemptNs_ = HighResClock::nowNs();
            return;
        }

        receiverEndpoint_ = sender;
        sessionId_        = pkt->header.sessionId;

        const std::string deviceName(pkt->deviceName,
            ::strnlen(pkt->deviceName, sizeof(pkt->deviceName)));

        if (callbacks_.onReceiverConnected)
            callbacks_.onReceiverConnected(sender, deviceName);

        setState(SessionState::Configuring);
        sendSessionConfig();
        connectAttemptNs_ = HighResClock::nowNs();
    }

    void handleSessionAck(const uint8_t* data, size_t size)
    {
        if (size < sizeof(Protocol::SessionAckPacket)) return;
        const auto* pkt = reinterpret_cast<const Protocol::SessionAckPacket*>(data);
        if (!pkt->accepted) {
            setState(SessionState::Idle);
            return;
        }

        lastPacketFromReceiverNs_ = HighResClock::nowNs();
        sessionId_                = pkt->header.sessionId;

        // Keep packet size aligned with DAW block size (set in setAudioParameters)
        const uint32_t block = static_cast<uint32_t>(
            std::max(blockSize_.load(std::memory_order_relaxed), 64));
        framesPerPacket_ = std::min(block, 256u);

        const bool wasStreaming = (state() == SessionState::Streaming);
        if (!wasStreaming) {
            audioSequenceNumber_ = 0;
        }
        setState(SessionState::Streaming);
        sendClockSync();
    }

    void handleHeartbeat(const uint8_t* data, size_t size, const EndPoint& sender)
    {
        if (size < sizeof(Protocol::HeartbeatPacket)) return;
        const auto* pkt = reinterpret_cast<const Protocol::HeartbeatPacket*>(data);

        lastPacketFromReceiverNs_ = HighResClock::nowNs();

        // Send ack
        Protocol::HeartbeatAckPacket ack{};
        ack.header.init(Protocol::PacketType::HeartbeatAck,
                        sequenceNumber_++, sessionId_);
        ack.originTimestampNs  = pkt->senderTimestampNs;
        ack.receiveTimestampNs = HighResClock::nowNs();
        ack.jitterBufferMs     = 0; // Plugin doesn't have a jitter buffer
        ack.networkQuality     = static_cast<uint8_t>(stats_.snapshot().quality);

        socket_.sendTo(receiverEndpoint_, &ack, sizeof(ack));
        stats_.onPacketSent();
    }

    void handleHeartbeatAck(const uint8_t* data, size_t size)
    {
        if (size < sizeof(Protocol::HeartbeatAckPacket)) return;
        lastPacketFromReceiverNs_ = HighResClock::nowNs();

        const auto* pkt = reinterpret_cast<const Protocol::HeartbeatAckPacket*>(data);
        const double latency = (HighResClock::nowNs() - pkt->originTimestampNs)
                               / 2'000'000.0;

        if (callbacks_.onLatencyUpdated)
            callbacks_.onLatencyUpdated(static_cast<float>(latency));
    }

    void handleClockSyncAck(const uint8_t* data, size_t size)
    {
        if (size < sizeof(Protocol::ClockSyncAckPacket)) return;
        const auto* pkt = reinterpret_cast<const Protocol::ClockSyncAckPacket*>(data);

        clockSync_.processAck(pkt->t1Ns, pkt->t2Ns, pkt->t3Ns);
        stats_.updateRtt(clockSync_.rttMs());
        lastPacketFromReceiverNs_ = HighResClock::nowNs();
    }

    void handleGoodbye()
    {
        setState(SessionState::Idle);
        receiverEndpoint_ = {};
        if (callbacks_.onReceiverDisconnected)
            callbacks_.onReceiverDisconnected();
    }

    void handleLatencyMode(const uint8_t* data, size_t size)
    {
        // LatencyMode packet - single byte payload
        if (size < sizeof(Protocol::PacketHeader) + 1) return;
        latencyMode_ = static_cast<Protocol::LatencyMode>(data[sizeof(Protocol::PacketHeader)]);
    }

    //==========================================================================
    // State management
    //==========================================================================

    void setState(SessionState newState)
    {
        const SessionState old = static_cast<SessionState>(
            state_.exchange(static_cast<int>(newState), std::memory_order_acq_rel));

        if (old != newState && callbacks_.onStateChanged)
            callbacks_.onStateChanged(newState);
    }

    //==========================================================================
    // Platform helpers
    //==========================================================================

    static std::string getComputerName()
    {
#ifdef _WIN32
        char name[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD size = sizeof(name);
        ::GetComputerNameA(name, &size);
        return name;
#else
        char name[256] = {};
        ::gethostname(name, sizeof(name) - 1);
        return name;
#endif
    }

    //==========================================================================
    // Members
    //==========================================================================

    // Audio parameters (atomic for cross-thread access)
    std::atomic<double>   sampleRate_{48000.0};
    std::atomic<int>      blockSize_{512};
    std::atomic<float>    signalPeakL_{0.0f};
    std::atomic<float>    signalPeakR_{0.0f};

    // Session state
    std::atomic<int>      state_{static_cast<int>(SessionState::Idle)};
    std::atomic<bool>     running_{false};
    std::atomic<bool>     disconnectRequested_{false};
    std::atomic<bool>     reconnectRequested_{false};
    std::atomic<bool>     hasNewEndpoint_{false};

    // Endpoint management
    std::mutex   endpointMutex_;
    EndPoint     pendingEndpoint_;
    EndPoint     receiverEndpoint_;

    // Protocol state
    uint32_t                 sequenceNumber_          = 0;
    uint32_t                 audioSequenceNumber_     = 0;
    uint32_t                 sessionId_               = 0x12345678;
    uint32_t                 framesPerPacket_         = 256;
    uint64_t                 totalFramesSent_         = 0;
    uint64_t                 lastPacketFromReceiverNs_= 0;
    uint64_t                 connectAttemptNs_        = 0;
    uint64_t                 lastSoftProbeNs_         = 0;
    Protocol::LatencyMode    latencyMode_             = Protocol::LatencyMode::Auto;

    // Subsystems
    UDPSocket           socket_;
    ClockSynchronizer   clockSync_;
    NetworkStats        stats_;
    AudioRingBuffer*    ringBuffer_ = nullptr;
    StreamerCallbacks   callbacks_;
    std::thread         thread_;
};

} // namespace MixBridge
