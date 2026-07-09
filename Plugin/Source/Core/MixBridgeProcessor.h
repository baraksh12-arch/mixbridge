#pragma once
//==============================================================================
// MixBridgeProcessor.h
// JUCE AudioProcessor subclass.
//
// Audio thread responsibilities:
//   - Capture stereo master output from processBlock()
//   - Write to lock-free ring buffer (NO mutexes, NO allocations, NO I/O)
//   - Track peak levels for metering
//
// All other work happens on the network thread.
//==============================================================================

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <cmath>

#include "../Network/NetworkStreamer.h"
#include "../Discovery/BonjourDiscovery.h"
#include "../../../Shared/RingBuffer/LockFreeRingBuffer.h"
#include "../../../Shared/Common/HighResClock.h"

namespace MixBridge {

//==============================================================================
// Parameters
//==============================================================================

namespace Params {
    static const juce::String kLatencyMode  = "latency_mode";
    static const juce::String kAutoConnect  = "auto_connect";
    static const juce::String kBypassStream = "bypass_stream";
}

//==============================================================================
// MixBridgeProcessor
//==============================================================================

class MixBridgeProcessor final : public juce::AudioProcessor,
                                 private juce::AudioProcessorParameter::Listener
{
public:
    MixBridgeProcessor();
    ~MixBridgeProcessor() override;

    //==========================================================================
    // AudioProcessor interface
    //==========================================================================

    const juce::String getName() const override { return "MixBridge"; }

    bool  acceptsMidi()  const override { return false; }
    bool  producesMidi() const override { return false; }
    bool  isMidiEffect() const override { return false; }

    double getTailLengthSeconds() const override { return 0.0; }

    int  getNumPrograms()                           override { return 1; }
    int  getCurrentProgram()                        override { return 0; }
    void setCurrentProgram(int)                     override {}
    const juce::String getProgramName(int)          override { return {}; }
    void changeProgramName(int, const juce::String&)override {}

    //==========================================================================
    // Prepare / Release
    //==========================================================================

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    //==========================================================================
    // AUDIO THREAD - the only performance-critical path.
    // Must NEVER block, allocate, or take mutexes.
    //==========================================================================
    void processBlock(juce::AudioBuffer<float>& buffer,
                      juce::MidiBuffer& midiMessages) override;

    //==========================================================================
    // State
    //==========================================================================

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==========================================================================
    // Plugin UI creation
    //==========================================================================

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    //==========================================================================
    // Public accessors for UI (safe to call from message thread)
    //==========================================================================

    SessionState getSessionState() const noexcept
    {
        return streamer_.state();
    }

    std::string getConnectedDeviceName() const
    {
        std::lock_guard<std::mutex> lock(deviceNameMutex_);
        return connectedDeviceName_;
    }

    float getCurrentLatencyMs() const noexcept
    {
        return currentLatencyMs_.load(std::memory_order_relaxed);
    }

    float getSignalPeakL() const noexcept
    {
        return displayPeakL_.load(std::memory_order_relaxed);
    }

    float getSignalPeakR() const noexcept
    {
        return displayPeakR_.load(std::memory_order_relaxed);
    }

    NetworkStats::Snapshot getNetworkStats() const
    {
        return streamer_.stats().snapshot();
    }

    std::vector<ReceiverInfo> getDiscoveredReceivers() const
    {
        return discovery_.getReceivers();
    }

    void connectToReceiver(const ReceiverInfo& receiver)
    {
        streamer_.setReceiverEndpoint({receiver.address, receiver.port});
    }

    void disconnect()
    {
        streamer_.disconnect();
    }

    void reconnect()
    {
        streamer_.reconnect();
    }

    juce::AudioProcessorValueTreeState& getParameters() { return parameters_; }

    void parameterValueChanged(int parameterIndex, float newValue) override;
    void parameterGestureChanged(int parameterIndex, bool gestureIsStarting) override;

private:
    //==========================================================================
    // Setup subsystems (called from prepare / constructor)
    //==========================================================================

    void initializeStreamer();
    void startDiscovery();

    //==========================================================================
    // Peak meter decay (called from audio thread - no allocations)
    //==========================================================================

    void updatePeakMeters(const juce::AudioBuffer<float>& buffer) noexcept;

    static float computePeak(const float* data, int numSamples) noexcept
    {
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            const float abs = std::fabs(data[i]);
            if (abs > peak) peak = abs;
        }
        return peak;
    }

    //==========================================================================
    // Members
    //==========================================================================

    // Parameter tree
    juce::AudioProcessorValueTreeState parameters_;

    // Audio ring buffer (between audio thread and network thread)
    AudioRingBuffer ringBuffer_;

    // Network subsystems
    NetworkStreamer   streamer_;
    BonjourDiscovery  discovery_;

    // Peak meters - written from audio thread, read from UI thread
    // Using atomic float for thread-safe access
    std::atomic<float> rawPeakL_{0.0f};
    std::atomic<float> rawPeakR_{0.0f};
    std::atomic<float> displayPeakL_{0.0f}; // With hold/decay
    std::atomic<float> displayPeakR_{0.0f};
    std::atomic<float> currentLatencyMs_{0.0f};

    // Peak hold state (accessed only from audio thread)
    float holdPeakL_   = 0.0f;
    float holdPeakR_   = 0.0f;
    int   holdCounterL_= 0;
    int   holdCounterR_= 0;

    static constexpr int kPeakHoldSamples  = 44100;   // 1 second @ 44.1kHz
    static constexpr float kDecayPerSample = 0.9999f; // Smooth decay

    // Session info (written from network thread callback, read from UI)
    mutable std::mutex  deviceNameMutex_;
    std::string         connectedDeviceName_;

    // Sample rate for metric calculations (set in prepareToPlay)
    double currentSampleRate_ = 48000.0;
    int    currentBlockSize_  = 512;

    // Bypass flag (from parameter, read in audio thread)
    std::atomic<bool> bypassStream_{false};

    // Auto-connect: automatically pick first discovered receiver
    std::atomic<bool> autoConnect_{true};
    bool              hasAutoConnected_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixBridgeProcessor)
};

} // namespace MixBridge
