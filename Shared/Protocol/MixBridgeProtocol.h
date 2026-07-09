#pragma once
//==============================================================================
// MixBridge Protocol v1.0
// Defines all packet formats, constants, and protocol state machines for
// the VST3 ↔ iPhone audio streaming system.
//
// Transport: UDP (best-effort, ordered via sequence numbers)
// Audio:     PCM Float32, stereo, lossless, no compression
// Discovery: Bonjour/mDNS (_mixbridge._udp.local)
//==============================================================================

#include <cstdint>
#include <cstring>
#include <array>

namespace MixBridge {
namespace Protocol {

//==============================================================================
// Constants
//==============================================================================

static constexpr uint16_t kDefaultPort          = 51234;
static constexpr uint32_t kMagicHeader          = 0x4D585247; // "MXRG"
static constexpr uint8_t  kProtocolVersion      = 1;
static constexpr char     kServiceType[]        = "_mixbridge._udp.";
static constexpr char     kServiceName[]        = "MixBridge";
static constexpr uint32_t kHeartbeatIntervalMs  = 500;
static constexpr uint32_t kConnectionTimeoutMs  = 10000;
static constexpr uint32_t kMaxPacketSize        = 65507; // Max UDP payload
static constexpr uint32_t kMaxSamplesPerPacket  = 4096;  // stereo * float32 max
static constexpr uint32_t kAudioPayloadMaxBytes = kMaxSamplesPerPacket * 2 * sizeof(float);

// Jitter buffer sizing (in ms)
static constexpr uint32_t kJitterBufferUltraLowMs = 2;
static constexpr uint32_t kJitterBufferLowMs      = 5;
static constexpr uint32_t kJitterBufferNormalMs   = 10;
static constexpr uint32_t kJitterBufferStableMs   = 20;

//==============================================================================
// Packet Types
//==============================================================================

enum class PacketType : uint8_t
{
    // Control
    Hello           = 0x01,  // Plugin → App: announce presence
    HelloAck        = 0x02,  // App → Plugin: acknowledge, send capabilities
    Goodbye         = 0x03,  // Either: graceful disconnect
    Heartbeat       = 0x04,  // Either: keep-alive ping
    HeartbeatAck    = 0x05,  // Either: pong
    SessionConfig   = 0x06,  // Plugin → App: stream parameters
    SessionAck      = 0x07,  // App → Plugin: ready to receive
    LatencyMode     = 0x08,  // App → Plugin: request latency mode change

    // Audio
    AudioFrame      = 0x10,  // Plugin → App: PCM audio payload

    // Clock sync (NTP-style two-way)
    ClockSync       = 0x20,  // Either: t1 timestamp
    ClockSyncAck    = 0x21,  // Either: t1 + t2 + t3 timestamps

    // Diagnostics
    StatsRequest    = 0x30,  // App → Plugin: request stats
    StatsResponse   = 0x31,  // Plugin → App: current stats
};

//==============================================================================
// Latency Mode
//==============================================================================

enum class LatencyMode : uint8_t
{
    Auto      = 0,
    UltraLow  = 1,
    Low       = 2,
    Normal    = 3,
    Stable    = 4,
};

//==============================================================================
// Network Quality
//==============================================================================

enum class NetworkQuality : uint8_t
{
    Unknown   = 0,
    Excellent = 1,
    Good      = 2,
    Fair      = 3,
    Poor      = 4,
};

//==============================================================================
// Packet Header (common to all packets, 16 bytes)
//==============================================================================

#pragma pack(push, 1)

struct PacketHeader
{
    uint32_t magic;           // Always kMagicHeader
    uint8_t  version;         // Protocol version
    uint8_t  type;            // PacketType
    uint16_t headerCRC;       // CRC16 of header bytes [0..13] (zeros for CRC field)
    uint32_t sequenceNumber;  // Monotonically increasing per sender
    uint32_t sessionId;       // Random 32-bit session identifier

    void init(PacketType t, uint32_t seq, uint32_t sid) noexcept
    {
        magic          = kMagicHeader;
        version        = kProtocolVersion;
        type           = static_cast<uint8_t>(t);
        headerCRC      = 0;
        sequenceNumber = seq;
        sessionId      = sid;
        headerCRC      = computeCRC();
    }

    bool isValid() const noexcept
    {
        if (magic   != kMagicHeader)     return false;
        if (version != kProtocolVersion) return false;
        uint16_t saved = headerCRC;
        const_cast<PacketHeader*>(this)->headerCRC = 0;
        uint16_t computed = computeCRC();
        const_cast<PacketHeader*>(this)->headerCRC = saved;
        return computed == saved;
    }

private:
    uint16_t computeCRC() const noexcept
    {
        // CRC-16/CCITT-FALSE
        const uint8_t* data = reinterpret_cast<const uint8_t*>(this);
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < sizeof(PacketHeader); ++i) {
            crc ^= static_cast<uint16_t>(data[i]) << 8;
            for (int b = 0; b < 8; ++b)
                crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
        return crc;
    }
};

static_assert(sizeof(PacketHeader) == 16, "PacketHeader must be 16 bytes");

//==============================================================================
// Hello Packet (Plugin → App)
//==============================================================================

struct HelloPacket
{
    PacketHeader header;
    uint64_t     senderTimestampNs;    // Sender wall-clock in nanoseconds
    char         pluginName[32];       // "MixBridge VST3\0..."
    char         computerName[64];     // OS hostname
    uint8_t      capabilities;         // Reserved, set to 0
    uint8_t      pad[3];
};

//==============================================================================
// HelloAck Packet (App → Plugin)
//==============================================================================

struct HelloAckPacket
{
    PacketHeader header;
    uint64_t     senderTimestampNs;
    char         appName[32];          // "MixBridge iOS\0..."
    char         deviceName[64];       // iPhone model
    uint8_t      capabilities;
    uint8_t      requestedLatencyMode; // LatencyMode enum
    uint8_t      pad[2];
};

//==============================================================================
// Goodbye Packet
//==============================================================================

struct GoodbyePacket
{
    PacketHeader header;
    uint8_t      reason;   // 0=normal, 1=error, 2=timeout
    uint8_t      pad[3];
};

//==============================================================================
// Heartbeat Packet
//==============================================================================

struct HeartbeatPacket
{
    PacketHeader header;
    uint64_t     senderTimestampNs;
    uint32_t     packetsLost;      // Running counter
    uint32_t     packetsSent;      // Running counter
    float        signalLevelL;     // Current peak dBFS (-inf..0)
    float        signalLevelR;
};

//==============================================================================
// HeartbeatAck
//==============================================================================

struct HeartbeatAckPacket
{
    PacketHeader header;
    uint64_t     originTimestampNs;  // Echo of sender's timestamp
    uint64_t     receiveTimestampNs; // When we received the heartbeat
    uint32_t     jitterBufferMs;     // Current jitter buffer depth
    uint8_t      networkQuality;     // NetworkQuality enum
    uint8_t      pad[3];
};

//==============================================================================
// SessionConfig Packet (Plugin → App, sent after HelloAck)
//==============================================================================

struct SessionConfigPacket
{
    PacketHeader header;
    uint32_t     sampleRate;         // e.g. 48000
    uint16_t     blockSize;          // DAW buffer size in frames
    uint8_t      channelCount;       // Always 2 for stereo
    uint8_t      bitsPerSample;      // Always 32 (Float32)
    uint32_t     framesPerPacket;    // How many stereo frames per AudioFrame
    uint32_t     nominalBitrateKbps; // e.g. 3072 for 48kHz float32 stereo
    uint8_t      latencyMode;        // LatencyMode
    uint8_t      pad[3];
};

//==============================================================================
// SessionAck Packet (App → Plugin)
//==============================================================================

struct SessionAckPacket
{
    PacketHeader header;
    uint8_t      accepted;       // 1 = accepted, 0 = rejected
    uint8_t      latencyMode;    // Confirmed latency mode
    uint16_t     jitterBufferMs; // Negotiated jitter buffer
    uint8_t      pad[4];
};

//==============================================================================
// AudioFrame Packet (Plugin → App)
// Variable length - header followed by interleaved PCM float32 data
// Total size = sizeof(AudioFrameHeader) + (frameCount * 2 * 4) bytes
//==============================================================================

struct AudioFrameHeader
{
    PacketHeader header;
    uint64_t     captureTimestampNs;  // When audio was captured (plugin clock)
    uint32_t     sampleRate;          // Redundant but allows sanity check
    uint32_t     frameCount;          // Number of stereo frames in payload
    uint64_t     sampleOffset;        // Global sample position (for sync)
    float        peakL;               // Peak amplitude this frame (0..1)
    float        peakR;
    uint32_t     payloadCRC;          // CRC32 of audio payload bytes
    uint8_t      pad[8];
    // Followed immediately by: float[frameCount * 2] (L,R,L,R,...)
};

static_assert(sizeof(AudioFrameHeader) == 60, "AudioFrameHeader size check");

//==============================================================================
// ClockSync Packet (NTP-style)
//==============================================================================

struct ClockSyncPacket
{
    PacketHeader header;
    uint64_t     t1Ns;   // Originate timestamp (sender's clock when sent)
};

struct ClockSyncAckPacket
{
    PacketHeader header;
    uint64_t     t1Ns;   // Echo of originate
    uint64_t     t2Ns;   // Receive timestamp (receiver's clock when received)
    uint64_t     t3Ns;   // Transmit timestamp (receiver's clock when sending ack)
    // Receiver computes: RTT = (t4-t1)-(t3-t2), offset = ((t2-t1)+(t3-t4))/2
};

//==============================================================================
// StatsResponse Packet
//==============================================================================

struct StatsResponsePacket
{
    PacketHeader header;
    uint64_t     uptimeMs;
    uint64_t     totalFramesSent;
    uint32_t     packetsSent;
    uint32_t     packetsLost;
    float        packetLossPercent;
    float        currentLatencyMs;
    float        jitterMs;
    float        signalPeakL;
    float        signalPeakR;
    uint32_t     sampleRate;
    uint32_t     bufferSize;
    uint8_t      networkQuality;
    uint8_t      latencyMode;
    uint8_t      pad[2];
};

#pragma pack(pop)

//==============================================================================
// Utility: CRC32 for audio payload validation
//==============================================================================

inline uint32_t crc32(const uint8_t* data, size_t length) noexcept
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFF;
}

//==============================================================================
// Packet size validator
//==============================================================================

inline bool validatePacketSize(PacketType type, size_t receivedBytes) noexcept
{
    switch (type) {
        case PacketType::Hello:        return receivedBytes == sizeof(HelloPacket);
        case PacketType::HelloAck:     return receivedBytes == sizeof(HelloAckPacket);
        case PacketType::Goodbye:      return receivedBytes == sizeof(GoodbyePacket);
        case PacketType::Heartbeat:    return receivedBytes == sizeof(HeartbeatPacket);
        case PacketType::HeartbeatAck: return receivedBytes == sizeof(HeartbeatAckPacket);
        case PacketType::SessionConfig:return receivedBytes == sizeof(SessionConfigPacket);
        case PacketType::SessionAck:   return receivedBytes == sizeof(SessionAckPacket);
        case PacketType::ClockSync:    return receivedBytes == sizeof(ClockSyncPacket);
        case PacketType::ClockSyncAck: return receivedBytes == sizeof(ClockSyncAckPacket);
        case PacketType::StatsResponse:return receivedBytes == sizeof(StatsResponsePacket);
        case PacketType::AudioFrame:
            return receivedBytes >= sizeof(AudioFrameHeader);
        default: return false;
    }
}

} // namespace Protocol
} // namespace MixBridge
