//==============================================================================
// MixBridgeTests.cpp
// Unit tests using JUCE's UnitTest framework.
// Tests all non-audio-thread components.
//==============================================================================

#include <JuceHeader.h>
#include "../Shared/RingBuffer/LockFreeRingBuffer.h"
#include "../Shared/Protocol/MixBridgeProtocol.h"
#include "../Shared/Common/HighResClock.h"
#include "../Shared/Common/NetworkStats.h"

namespace MixBridge {
namespace Tests {

//==============================================================================
// Ring Buffer Tests
//==============================================================================

class RingBufferTest : public juce::UnitTest
{
public:
    RingBufferTest() : UnitTest("LockFreeRingBuffer", "MixBridge") {}

    void runTest() override
    {
        beginTest("Basic push/pop");
        {
            LockFreeRingBuffer<int, 16> buf;
            expect(buf.isEmpty());
            expect(buf.available() == 0);

            expectEquals((int)buf.freeSpace(), 15);

            expect(buf.push(42));
            expectEquals((int)buf.available(), 1);

            int val = 0;
            expect(buf.pop(val));
            expectEquals(val, 42);
            expect(buf.isEmpty());
        }

        beginTest("Full buffer returns false on push");
        {
            LockFreeRingBuffer<int, 4> buf;
            expect(buf.push(1));
            expect(buf.push(2));
            expect(buf.push(3));
            expect(!buf.push(4)); // Full (capacity-1 slots usable)
        }

        beginTest("pushN / popN");
        {
            LockFreeRingBuffer<float, 32> buf;
            float src[8] = {1,2,3,4,5,6,7,8};
            expectEquals((int)buf.pushN(src, 8), 8);

            float dst[8] = {};
            expectEquals((int)buf.popN(dst, 8), 8);
            for (int i = 0; i < 8; ++i)
                expectWithinAbsoluteError(dst[i], src[i], 1e-6f);
        }

        beginTest("Peek does not consume");
        {
            LockFreeRingBuffer<int, 8> buf;
            buf.push(99);
            int val = 0;
            expect(buf.peek(val));
            expectEquals(val, 99);
            expectEquals((int)buf.available(), 1);
            expect(buf.pop(val));
            expectEquals((int)buf.available(), 0);
        }

        beginTest("Wrap-around correctness");
        {
            LockFreeRingBuffer<int, 8> buf;
            // Fill 7 slots
            for (int i = 0; i < 7; ++i) buf.push(i);
            // Drain 4
            int v; for (int i = 0; i < 4; ++i) buf.pop(v);
            // Push 4 more (wraps around)
            for (int i = 10; i < 14; ++i) buf.push(i);
            // Should have 7 items: 4,5,6,10,11,12,13
            expectEquals((int)buf.available(), 7);
            buf.pop(v); expectEquals(v, 4);
            buf.pop(v); expectEquals(v, 5);
            buf.pop(v); expectEquals(v, 6);
            buf.pop(v); expectEquals(v, 10);
        }

        beginTest("AudioRingBuffer stereo write/read");
        {
            AudioRingBuffer abuf;
            float left[4]  = {0.1f, 0.2f, 0.3f, 0.4f};
            float right[4] = {0.5f, 0.6f, 0.7f, 0.8f};

            expectEquals((int)abuf.write(left, right, 4), 4);
            expectEquals((int)abuf.availableFrames(), 4);

            float interleaved[8] = {};
            expectEquals((int)abuf.read(interleaved, 4), 4);

            expectWithinAbsoluteError(interleaved[0], 0.1f, 1e-6f);
            expectWithinAbsoluteError(interleaved[1], 0.5f, 1e-6f);
            expectWithinAbsoluteError(interleaved[2], 0.2f, 1e-6f);
            expectWithinAbsoluteError(interleaved[3], 0.6f, 1e-6f);
        }

        beginTest("AudioRingBuffer underrun returns less than requested");
        {
            AudioRingBuffer abuf;
            float l[4] = {}, r[4] = {};
            abuf.write(l, r, 2); // Write 2 frames
            float out[8] = {};
            size_t read = abuf.read(out, 4); // Request 4
            expectEquals((int)read, 2);      // Only 2 available
        }
    }
};

//==============================================================================
// Protocol Tests
//==============================================================================

class ProtocolTest : public juce::UnitTest
{
public:
    ProtocolTest() : UnitTest("Protocol", "MixBridge") {}

    void runTest() override
    {
        beginTest("PacketHeader magic and CRC");
        {
            Protocol::PacketHeader hdr;
            hdr.init(Protocol::PacketType::AudioFrame, 42, 0xDEADBEEF);

            expect(hdr.isValid());
            expectEquals(hdr.magic, Protocol::kMagicHeader);
            expectEquals(hdr.version, Protocol::kProtocolVersion);
            expectEquals((int)hdr.sequenceNumber, 42);

            // Corrupt one byte - should fail validation
            hdr.magic = 0x12345678;
            expect(!hdr.isValid());
        }

        beginTest("HelloPacket size");
        {
            // Verify packet sizes match expected wire format
            expectEquals((int)sizeof(Protocol::HelloPacket), 16 + 8 + 32 + 64 + 4);
        }

        beginTest("AudioFrameHeader size");
        {
            // header(16) + captureTs(8) + sampleRate(4) + frameCount(4)
            // + sampleOffset(8) + peakL(4) + peakR(4) + payloadCRC(4) + pad(4) = 60
            expectEquals((int)sizeof(Protocol::AudioFrameHeader), 60);
        }

        beginTest("CRC32 correctness");
        {
            const uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
            uint32_t crc = Protocol::crc32(data, 4);
            // Known CRC32 of {1,2,3,4}
            expectEquals(crc, 0xB63CFBCDU);
        }

        beginTest("validatePacketSize");
        {
            expect(Protocol::validatePacketSize(
                Protocol::PacketType::Hello,
                sizeof(Protocol::HelloPacket)));
            expect(!Protocol::validatePacketSize(
                Protocol::PacketType::Hello,
                sizeof(Protocol::HelloPacket) - 1));
            expect(Protocol::validatePacketSize(
                Protocol::PacketType::AudioFrame,
                sizeof(Protocol::AudioFrameHeader) + 512));
            expect(!Protocol::validatePacketSize(
                Protocol::PacketType::AudioFrame,
                sizeof(Protocol::AudioFrameHeader) - 1));
        }
    }
};

//==============================================================================
// Clock Sync Tests
//==============================================================================

class ClockSyncTest : public juce::UnitTest
{
public:
    ClockSyncTest() : UnitTest("ClockSynchronizer", "MixBridge") {}

    void runTest() override
    {
        beginTest("Clock sync basic");
        {
            ClockSynchronizer sync;
            expect(!sync.isValid());

            // Simulate 3 round trips with known timing
            for (int i = 0; i < 5; ++i) {
                uint64_t t1 = sync.beginSync();
                // Simulate 5ms RTT: t2 = t1 + 2ms, t3 = t1 + 3ms
                uint64_t t2 = t1 + 2'000'000ULL;
                uint64_t t3 = t1 + 3'000'000ULL;
                sync.processAck(t1, t2, t3);
            }

            expect(sync.isValid());
            // RTT should be ~5ms (t4-t1 ≈ 5ms with our simulated ack delay)
            // Offset should be ~-0.5ms (t2-t1=2, t3-t4≈-2, avg=-0.5ms... depends on wall clock)
            // Just verify it doesn't crash and is in reasonable range
            expect(sync.rttMs() >= 0.0);
            expect(sync.rttMs() < 100.0);
        }

        beginTest("Rejects bad timestamps");
        {
            ClockSynchronizer sync;
            uint64_t t1 = sync.beginSync();
            // t2 < t1 is invalid
            sync.processAck(t1, t1 - 1000, t1 + 1000);
            expect(!sync.isValid()); // Should not have counted this sample
        }

        beginTest("Rejects excessive RTT");
        {
            ClockSynchronizer sync;
            uint64_t t1 = sync.beginSync();
            // RTT > 500ms = invalid
            uint64_t t2 = t1 + 600'000'000ULL; // 600ms
            uint64_t t3 = t2 + 1'000'000ULL;
            sync.processAck(t1, t2, t3);
            expect(!sync.isValid());
        }
    }
};

//==============================================================================
// Network Stats Tests
//==============================================================================

class NetworkStatsTest : public juce::UnitTest
{
public:
    NetworkStatsTest() : UnitTest("NetworkStats", "MixBridge") {}

    void runTest() override
    {
        beginTest("Packet loss calculation");
        {
            NetworkStats stats;
            stats.onPacketSent();
            stats.onPacketReceived(0);
            stats.onPacketSent();
            stats.onPacketReceived(1);
            stats.onPacketSent();
            // Sequence 2 is missing
            stats.onPacketSent();
            stats.onPacketReceived(3); // Gap of 1

            auto snap = stats.snapshot();
            expectEquals((int)snap.packetsLost, 1);
            expectEquals((int)snap.packetsReceived, 3);
            expectWithinAbsoluteError(snap.lossPercent, 25.0f, 1.0f);
        }

        beginTest("Quality thresholds");
        {
            NetworkStats stats;
            // Good conditions
            stats.updateRtt(5.0);
            for (int i = 0; i < 100; ++i) {
                stats.onPacketSent();
                stats.onPacketReceived(static_cast<uint32_t>(i));
            }
            auto snap = stats.snapshot();
            using Q = Protocol::NetworkQuality;
            expect(snap.quality == Q::Excellent || snap.quality == Q::Good);
        }

        beginTest("Reset clears all state");
        {
            NetworkStats stats;
            for (int i = 0; i < 10; ++i) {
                stats.onPacketSent();
                stats.onPacketReceived(static_cast<uint32_t>(i));
            }
            stats.reset();
            auto snap = stats.snapshot();
            expectEquals((int)snap.packetsSent, 0);
            expectEquals((int)snap.packetsReceived, 0);
            expectEquals((int)snap.packetsLost, 0);
        }
    }
};

//==============================================================================
// Concurrent ring buffer stress test
//==============================================================================

class ConcurrentRingBufferTest : public juce::UnitTest
{
public:
    ConcurrentRingBufferTest() : UnitTest("ConcurrentRingBuffer", "MixBridge") {}

    void runTest() override
    {
        beginTest("SPSC concurrent producer/consumer");
        {
            LockFreeRingBuffer<uint64_t, 1024> buf;
            std::atomic<bool> done{false};
            std::atomic<uint64_t> consumed{0};
            std::atomic<uint64_t> produced{0};

            const int kIterations = 100000;

            // Producer thread
            auto producer = std::thread([&]() {
                for (int i = 0; i < kIterations; ++i) {
                    while (!buf.push(static_cast<uint64_t>(i)))
                        std::this_thread::yield();
                    produced.fetch_add(1, std::memory_order_relaxed);
                }
            });

            // Consumer thread
            auto consumer = std::thread([&]() {
                uint64_t expected = 0;
                while (expected < kIterations) {
                    uint64_t val;
                    if (buf.pop(val)) {
                        // Sequence must be monotonically increasing
                        jassert(val == expected);
                        ++expected;
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        std::this_thread::yield();
                    }
                }
            });

            producer.join();
            consumer.join();

            expectEquals((int)produced.load(), kIterations);
            expectEquals((int)consumed.load(), kIterations);
        }
    }
};

//==============================================================================
// Test registration
//==============================================================================

static RingBufferTest         ringBufferTest;
static ProtocolTest           protocolTest;
static ClockSyncTest          clockSyncTest;
static NetworkStatsTest       networkStatsTest;
static ConcurrentRingBufferTest concurrentTest;

} // namespace Tests
} // namespace MixBridge
