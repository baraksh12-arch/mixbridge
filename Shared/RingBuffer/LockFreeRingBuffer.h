#pragma once
//==============================================================================
// LockFreeRingBuffer.h
// Single-Producer Single-Consumer lock-free ring buffer.
//
// Design goals:
//   - Zero allocations after construction
//   - Zero locks / mutexes
//   - Safe for use in real-time audio threads
//   - Correct memory ordering via C++11 atomics
//   - Cache-line aligned to prevent false sharing
//
// Template parameter T must be trivially copyable.
// Capacity must be a power of two for efficient modulo via bitmask.
//==============================================================================

#include <atomic>
#include <cstring>
#include <type_traits>
#include <cassert>

namespace MixBridge {

template<typename T, size_t Capacity>
class LockFreeRingBuffer
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "LockFreeRingBuffer requires trivially copyable type");
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    LockFreeRingBuffer() noexcept
        : writeIndex_(0), readIndex_(0)
    {
    }

    // Not copyable or movable - contains atomics
    LockFreeRingBuffer(const LockFreeRingBuffer&)            = delete;
    LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer(LockFreeRingBuffer&&)                 = delete;
    LockFreeRingBuffer& operator=(LockFreeRingBuffer&&)      = delete;

    //==========================================================================
    // Push one item (Producer thread only)
    // Returns true if item was written, false if buffer was full.
    //==========================================================================
    [[nodiscard]] bool push(const T& item) noexcept
    {
        const size_t writePos = writeIndex_.load(std::memory_order_relaxed);
        const size_t nextPos  = (writePos + 1) & kMask;

        if (nextPos == readIndex_.load(std::memory_order_acquire))
            return false; // Full

        buffer_[writePos] = item;
        writeIndex_.store(nextPos, std::memory_order_release);
        return true;
    }

    //==========================================================================
    // Push N items from a pointer (Producer thread only)
    // Returns number of items actually written.
    //==========================================================================
    size_t pushN(const T* items, size_t count) noexcept
    {
        size_t written = 0;
        while (written < count && push(items[written]))
            ++written;
        return written;
    }

    //==========================================================================
    // Pop one item (Consumer thread only)
    // Returns true if item was read, false if buffer was empty.
    //==========================================================================
    [[nodiscard]] bool pop(T& item) noexcept
    {
        const size_t readPos = readIndex_.load(std::memory_order_relaxed);

        if (readPos == writeIndex_.load(std::memory_order_acquire))
            return false; // Empty

        item = buffer_[readPos];
        readIndex_.store((readPos + 1) & kMask, std::memory_order_release);
        return true;
    }

    //==========================================================================
    // Pop N items into a pointer (Consumer thread only)
    // Returns number of items actually read.
    //==========================================================================
    size_t popN(T* items, size_t count) noexcept
    {
        size_t read = 0;
        while (read < count && pop(items[read]))
            ++read;
        return read;
    }

    //==========================================================================
    // Peek at next item without consuming (Consumer thread only)
    //==========================================================================
    [[nodiscard]] bool peek(T& item) const noexcept
    {
        const size_t readPos = readIndex_.load(std::memory_order_relaxed);

        if (readPos == writeIndex_.load(std::memory_order_acquire))
            return false;

        item = buffer_[readPos];
        return true;
    }

    //==========================================================================
    // Available items to read (approximate - can be stale on other thread)
    //==========================================================================
    size_t available() const noexcept
    {
        const size_t w = writeIndex_.load(std::memory_order_acquire);
        const size_t r = readIndex_.load(std::memory_order_acquire);
        return (w - r) & kMask;
    }

    //==========================================================================
    // Free space available to write (approximate)
    //==========================================================================
    size_t freeSpace() const noexcept
    {
        return Capacity - 1 - available();
    }

    //==========================================================================
    // Is buffer empty? (approximate)
    //==========================================================================
    bool isEmpty() const noexcept
    {
        return writeIndex_.load(std::memory_order_acquire) ==
               readIndex_.load(std::memory_order_acquire);
    }

    //==========================================================================
    // Reset (call from single thread only, not thread-safe)
    //==========================================================================
    void reset() noexcept
    {
        writeIndex_.store(0, std::memory_order_relaxed);
        readIndex_.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    static constexpr size_t capacity() noexcept { return Capacity; }

private:
    static constexpr size_t kMask = Capacity - 1;

    // Cache-line separation to avoid false sharing between producer and consumer
    alignas(64) std::atomic<size_t> writeIndex_;
    alignas(64) std::atomic<size_t> readIndex_;
    alignas(64) T buffer_[Capacity];
};

//==============================================================================
// AudioRingBuffer - specialised for raw audio samples (float)
// Designed so audio thread writes, network thread reads.
//
// Capacity = 65536 floats = ~0.7 sec at 48kHz stereo
//==============================================================================

class AudioRingBuffer
{
public:
    static constexpr size_t kCapacity = 65536; // must be power of two

    AudioRingBuffer() noexcept : writeIdx_(0), readIdx_(0) {}

    AudioRingBuffer(const AudioRingBuffer&)            = delete;
    AudioRingBuffer& operator=(const AudioRingBuffer&) = delete;

    //==========================================================================
    // Write interleaved stereo samples.
    // Called from audio thread.
    // Returns number of frames written.
    //==========================================================================
    size_t write(const float* left, const float* right, size_t frameCount) noexcept
    {
        const size_t writePos  = writeIdx_.load(std::memory_order_relaxed);
        const size_t readPos   = readIdx_.load(std::memory_order_acquire);
        const size_t available = (kCapacity - ((writePos - readPos) & kMask)) / 2;
        const size_t toWrite   = frameCount < available ? frameCount : available;

        for (size_t i = 0; i < toWrite; ++i) {
            const size_t base = (writePos + i * 2) & kMask;
            buffer_[base]     = left[i];
            buffer_[base + 1] = right[i];
        }

        writeIdx_.store((writePos + toWrite * 2) & kMask, std::memory_order_release);
        return toWrite;
    }

    //==========================================================================
    // Read interleaved samples.
    // Called from network thread.
    // Returns number of frames read.
    //==========================================================================
    size_t read(float* interleaved, size_t frameCount) noexcept
    {
        const size_t readPos   = readIdx_.load(std::memory_order_relaxed);
        const size_t writePos  = writeIdx_.load(std::memory_order_acquire);
        const size_t avail     = ((writePos - readPos) & kMask) / 2;
        const size_t toRead    = frameCount < avail ? frameCount : avail;

        for (size_t i = 0; i < toRead; ++i) {
            const size_t base    = (readPos + i * 2) & kMask;
            interleaved[i * 2]   = buffer_[base];
            interleaved[i * 2 + 1] = buffer_[base + 1];
        }

        readIdx_.store((readPos + toRead * 2) & kMask, std::memory_order_release);
        return toRead;
    }

    size_t availableFrames() const noexcept
    {
        const size_t w = writeIdx_.load(std::memory_order_acquire);
        const size_t r = readIdx_.load(std::memory_order_acquire);
        return ((w - r) & kMask) / 2;
    }

    void reset() noexcept
    {
        writeIdx_.store(0, std::memory_order_relaxed);
        readIdx_.store(0, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

private:
    static constexpr size_t kMask = kCapacity - 1;
    alignas(64) std::atomic<size_t> writeIdx_;
    alignas(64) std::atomic<size_t> readIdx_;
    alignas(64) float buffer_[kCapacity];
};

} // namespace MixBridge
