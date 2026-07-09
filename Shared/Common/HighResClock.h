#pragma once
//==============================================================================
// HighResClock.h
// Cross-platform nanosecond-precision monotonic clock utilities.
// Used for packet timestamps, clock synchronization, and jitter measurement.
//==============================================================================

#include <cstdint>
#include <chrono>

namespace MixBridge {

//==============================================================================
// HighResClock
// Provides nanosecond timestamps. Uses std::chrono::steady_clock which is
// guaranteed monotonic (never goes backward, even across DST changes).
//==============================================================================

class HighResClock
{
public:
    using TimePoint = std::chrono::time_point<std::chrono::steady_clock>;
    using Duration  = std::chrono::nanoseconds;

    //==========================================================================
    // Current time as nanoseconds since an arbitrary epoch.
    // Guaranteed to be monotonically non-decreasing.
    //==========================================================================
    static uint64_t nowNs() noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }

    //==========================================================================
    // Current time as milliseconds.
    //==========================================================================
    static uint64_t nowMs() noexcept
    {
        return nowNs() / 1'000'000ULL;
    }

    //==========================================================================
    // Elapsed milliseconds since a given timestamp.
    //==========================================================================
    static uint64_t elapsedMs(uint64_t sinceNs) noexcept
    {
        const uint64_t now = nowNs();
        if (now < sinceNs) return 0;
        return (now - sinceNs) / 1'000'000ULL;
    }

    //==========================================================================
    // Convert nanoseconds delta to milliseconds (floating point).
    //==========================================================================
    static double nsToMs(int64_t ns) noexcept
    {
        return static_cast<double>(ns) / 1'000'000.0;
    }

    static double nsToMs(uint64_t ns) noexcept
    {
        return static_cast<double>(ns) / 1'000'000.0;
    }
};

//==============================================================================
// ClockSynchronizer
// Implements NTP-style two-way clock offset estimation.
//
// Protocol:
//   1. Sender sends ClockSync(t1)
//   2. Receiver responds with ClockSyncAck(t1, t2, t3)
//   3. Sender records t4, computes:
//       RTT    = (t4 - t1) - (t3 - t2)
//       offset = ((t2 - t1) + (t3 - t4)) / 2
//
// The offset can be applied to convert remote timestamps to local time.
//==============================================================================

class ClockSynchronizer
{
public:
    ClockSynchronizer() noexcept
        : offsetNs_(0)
        , rttNs_(0)
        , jitterNs_(0)
        , sampleCount_(0)
        , pendingT1_(0)
    {
    }

    //==========================================================================
    // Call before sending ClockSync. Records t1 and returns it.
    //==========================================================================
    uint64_t beginSync() noexcept
    {
        pendingT1_ = HighResClock::nowNs();
        return pendingT1_;
    }

    //==========================================================================
    // Call when ClockSyncAck is received. t1 = echo, t2 = receiver receive,
    // t3 = receiver send. Records t4 internally.
    //==========================================================================
    void processAck(uint64_t t1, uint64_t t2, uint64_t t3) noexcept
    {
        const uint64_t t4 = HighResClock::nowNs();

        // All timestamps should be monotonically sane
        if (t4 < t1 || t2 < t1 || t3 < t2) return;

        const int64_t rtt    = static_cast<int64_t>((t4 - t1) - (t3 - t2));
        const int64_t offset = (static_cast<int64_t>(t2 - t1) +
                                static_cast<int64_t>(t3 - t4)) / 2;

        // Reject obviously bad samples (RTT > 500ms)
        if (rtt < 0 || rtt > 500'000'000LL) return;

        // Running average with jitter tracking
        if (sampleCount_ == 0) {
            offsetNs_ = offset;
            rttNs_    = rtt;
        } else {
            // Exponential moving average, α = 0.125
            offsetNs_ = offsetNs_ + (offset - offsetNs_) / 8;
            rttNs_    = rttNs_    + (rtt    - rttNs_)    / 8;
        }

        // Jitter = mean absolute deviation from rtt
        const int64_t rttDev = (rtt > rttNs_) ? (rtt - rttNs_) : (rttNs_ - rtt);
        jitterNs_ = jitterNs_ + (rttDev - jitterNs_) / 8;

        ++sampleCount_;
    }

    //==========================================================================
    // Convert a remote timestamp (nanoseconds on remote clock) to local time.
    //==========================================================================
    uint64_t remoteToLocal(uint64_t remoteNs) const noexcept
    {
        return static_cast<uint64_t>(static_cast<int64_t>(remoteNs) - offsetNs_);
    }

    //==========================================================================
    // Convert a local timestamp to remote clock.
    //==========================================================================
    uint64_t localToRemote(uint64_t localNs) const noexcept
    {
        return static_cast<uint64_t>(static_cast<int64_t>(localNs) + offsetNs_);
    }

    int64_t  offsetNs()   const noexcept { return offsetNs_;  }
    int64_t  rttNs()      const noexcept { return rttNs_;     }
    int64_t  jitterNs()   const noexcept { return jitterNs_;  }
    uint32_t sampleCount()const noexcept { return sampleCount_; }

    double rttMs()    const noexcept { return HighResClock::nsToMs(rttNs_);    }
    double jitterMs() const noexcept { return HighResClock::nsToMs(jitterNs_); }

    bool isValid()    const noexcept { return sampleCount_ >= 3; }

    void reset() noexcept
    {
        offsetNs_    = 0;
        rttNs_       = 0;
        jitterNs_    = 0;
        sampleCount_ = 0;
        pendingT1_   = 0;
    }

private:
    int64_t  offsetNs_;
    int64_t  rttNs_;
    int64_t  jitterNs_;
    uint32_t sampleCount_;
    uint64_t pendingT1_;
};

} // namespace MixBridge
