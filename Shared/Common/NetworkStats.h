#pragma once
//==============================================================================
// NetworkStats.h
// Real-time network quality metrics tracker.
// Thread-safe via atomics. Updated on network thread, read on any thread.
//==============================================================================

#include <atomic>
#include <cstdint>
#include <algorithm>
#include "HighResClock.h"

namespace MixBridge {

class NetworkStats
{
public:
    NetworkStats() noexcept { reset(); }

    //==========================================================================
    // Called by sender when a packet is dispatched.
    //==========================================================================
    void onPacketSent() noexcept
    {
        packetsSent_.fetch_add(1, std::memory_order_relaxed);
    }

    //==========================================================================
    // Called by receiver when a packet arrives.
    // sequenceNumber must be monotonically increasing from sender.
    //==========================================================================
    void onPacketReceived(uint32_t sequenceNumber) noexcept
    {
        packetsReceived_.fetch_add(1, std::memory_order_relaxed);

        const uint32_t last = lastSequenceReceived_.load(std::memory_order_relaxed);

        if (last != kNoSequence) {
            const uint32_t expected = last + 1;
            if (sequenceNumber > expected) {
                // Gap detected - packets dropped
                const uint32_t lost = sequenceNumber - expected;
                packetsLost_.fetch_add(lost, std::memory_order_relaxed);
            }
        }

        lastSequenceReceived_.store(sequenceNumber, std::memory_order_relaxed);

        // Arrival time jitter measurement (RFC 3550)
        const uint64_t now = HighResClock::nowNs();
        const uint64_t last_arrival = lastArrivalNs_.load(std::memory_order_relaxed);

        if (last_arrival != 0) {
            const int64_t transit = static_cast<int64_t>(now - last_arrival);
            const int64_t prev    = static_cast<int64_t>(
                prevTransitNs_.load(std::memory_order_relaxed));
            const int64_t d       = transit - prev;
            const int64_t absD    = (d < 0) ? -d : d;

            // Jitter = running average of |transit difference|
            int64_t jitter = jitterNs_.load(std::memory_order_relaxed);
            jitter += (absD - jitter) / 16;
            jitterNs_.store(jitter, std::memory_order_relaxed);
            prevTransitNs_.store(static_cast<uint64_t>(transit), std::memory_order_relaxed);
        }

        lastArrivalNs_.store(now, std::memory_order_relaxed);
    }

    //==========================================================================
    // Update RTT from clock sync result.
    //==========================================================================
    void updateRtt(double rttMs) noexcept
    {
        // Store as integer microseconds for atomicity
        const uint32_t us = static_cast<uint32_t>(rttMs * 1000.0);
        rttUs_.store(us, std::memory_order_relaxed);
    }

    //==========================================================================
    // Snapshot all statistics (safe to call from any thread).
    //==========================================================================
    struct Snapshot
    {
        uint64_t packetsSent;
        uint64_t packetsReceived;
        uint64_t packetsLost;
        float    lossPercent;
        double   rttMs;
        double   jitterMs;
        Protocol::NetworkQuality quality;
    };

    Snapshot snapshot() const noexcept
    {
        Snapshot s;
        s.packetsSent     = packetsSent_.load(std::memory_order_relaxed);
        s.packetsReceived = packetsReceived_.load(std::memory_order_relaxed);
        s.packetsLost     = packetsLost_.load(std::memory_order_relaxed);

        const uint64_t total = s.packetsLost + s.packetsReceived;
        s.lossPercent = (total > 0)
            ? static_cast<float>(s.packetsLost) / static_cast<float>(total) * 100.0f
            : 0.0f;

        s.rttMs    = static_cast<double>(rttUs_.load(std::memory_order_relaxed)) / 1000.0;
        s.jitterMs = HighResClock::nsToMs(
            static_cast<int64_t>(jitterNs_.load(std::memory_order_relaxed)));

        s.quality = computeQuality(s.lossPercent, s.rttMs, s.jitterMs);
        return s;
    }

    float lossPercent() const noexcept
    {
        const uint64_t lost = packetsLost_.load(std::memory_order_relaxed);
        const uint64_t recv = packetsReceived_.load(std::memory_order_relaxed);
        const uint64_t total = lost + recv;
        return (total > 0)
            ? static_cast<float>(lost) / static_cast<float>(total) * 100.0f
            : 0.0f;
    }

    void reset() noexcept
    {
        packetsSent_.store(0, std::memory_order_relaxed);
        packetsReceived_.store(0, std::memory_order_relaxed);
        packetsLost_.store(0, std::memory_order_relaxed);
        lastSequenceReceived_.store(kNoSequence, std::memory_order_relaxed);
        jitterNs_.store(0, std::memory_order_relaxed);
        rttUs_.store(0, std::memory_order_relaxed);
        lastArrivalNs_.store(0, std::memory_order_relaxed);
        prevTransitNs_.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr uint32_t kNoSequence = 0xFFFFFFFF;

    static Protocol::NetworkQuality computeQuality(float loss, double rtt, double jitter) noexcept
    {
        using Q = Protocol::NetworkQuality;
        if (loss > 5.0f || rtt > 100.0 || jitter > 30.0) return Q::Poor;
        if (loss > 1.0f || rtt > 50.0  || jitter > 10.0) return Q::Fair;
        if (loss > 0.1f || rtt > 20.0  || jitter > 5.0)  return Q::Good;
        return Q::Excellent;
    }

    alignas(64) std::atomic<uint64_t> packetsSent_{0};
    alignas(64) std::atomic<uint64_t> packetsReceived_{0};
    alignas(64) std::atomic<uint64_t> packetsLost_{0};
    alignas(64) std::atomic<uint32_t> lastSequenceReceived_{kNoSequence};
    alignas(64) std::atomic<int64_t>  jitterNs_{0};
    alignas(64) std::atomic<uint32_t> rttUs_{0};
    alignas(64) std::atomic<uint64_t> lastArrivalNs_{0};
    alignas(64) std::atomic<uint64_t> prevTransitNs_{0};
};

} // namespace MixBridge
