/*
  ==============================================================================
  MixBridgeProcessor.cpp
  Full implementation of the VST3 audio processor.
  ==============================================================================
*/

#include "MixBridgeProcessor.h"
#include "../UI/MixBridgeEditor.h"
namespace MixBridge {

//==============================================================================
// Parameter layout
//==============================================================================

static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        Params::kLatencyMode,
        "Latency Mode",
        juce::StringArray{"Auto", "Ultra Low", "Low", "Normal", "Stable"},
        0));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        Params::kAutoConnect,
        "Auto Connect",
        true));

    params.push_back(std::make_unique<juce::AudioParameterBool>(
        Params::kBypassStream,
        "Bypass Stream",
        false));

    return {params.begin(), params.end()};
}

//==============================================================================
// Constructor
//==============================================================================

MixBridgeProcessor::MixBridgeProcessor()
    : AudioProcessor(BusesProperties()
        .withInput ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    , parameters_(*this, nullptr, "MixBridge", createParameterLayout())
{
    // Listen for bypass parameter changes
    if (auto* param = parameters_.getParameter(Params::kBypassStream))
        param->addListener(this);

    if (auto* param = parameters_.getParameter(Params::kAutoConnect))
        param->addListener(this);

    // Generate a unique session ID from current time
    const uint32_t sessionId = static_cast<uint32_t>(
        HighResClock::nowNs() & 0xFFFFFFFF);

    initializeStreamer();
    startDiscovery();
}

MixBridgeProcessor::~MixBridgeProcessor()
{
    streamer_.stop();
    discovery_.stop();
}

//==============================================================================
// initializeStreamer
//==============================================================================

void MixBridgeProcessor::initializeStreamer()
{
    StreamerCallbacks cb;

    cb.onStateChanged = [this](SessionState state) {
        if (state == SessionState::Idle)
            hasAutoConnected_ = false;

        // Notify editor if open
        if (auto* editor = dynamic_cast<MixBridgeEditor*>(getActiveEditor()))
            juce::MessageManager::callAsync([editor] { editor->refreshState(); });
    };

    cb.onReceiverConnected = [this](const EndPoint& ep,
                                     const std::string& deviceName) {
        {
            std::lock_guard<std::mutex> lock(deviceNameMutex_);
            connectedDeviceName_ = deviceName;
        }
        if (auto* editor = dynamic_cast<MixBridgeEditor*>(getActiveEditor()))
            juce::MessageManager::callAsync([editor] { editor->refreshState(); });
    };

    cb.onReceiverDisconnected = [this]() {
        {
            std::lock_guard<std::mutex> lock(deviceNameMutex_);
            connectedDeviceName_.clear();
        }
        if (auto* editor = dynamic_cast<MixBridgeEditor*>(getActiveEditor()))
            juce::MessageManager::callAsync([editor] { editor->refreshState(); });
    };

    cb.onLatencyUpdated = [this](float latencyMs) {
        currentLatencyMs_.store(latencyMs, std::memory_order_relaxed);
    };

    cb.onStatsUpdated = [this](const NetworkStats::Snapshot& /*snap*/) {
        // Stats available via getNetworkStats()
    };

    streamer_.start(ringBuffer_, cb);
}

//==============================================================================
// startDiscovery
//==============================================================================

void MixBridgeProcessor::startDiscovery()
{
    DiscoveryCallbacks cb;

    cb.onReceiverFound = [this](const ReceiverInfo& receiver) {
        // Auto-connect to first discovered receiver if enabled
        const bool autoConnect = autoConnect_.load(std::memory_order_relaxed);
        if (autoConnect && !hasAutoConnected_ &&
            streamer_.state() == SessionState::Idle &&
            !discovery_.isOwnService(receiver.name)) {
            hasAutoConnected_ = true;
            connectToReceiver(receiver);
        }

        if (auto* editor = dynamic_cast<MixBridgeEditor*>(getActiveEditor()))
            juce::MessageManager::callAsync([editor] { editor->refreshReceiverList(); });
    };

    cb.onReceiverLost = [this](const std::string&) {
        if (auto* editor = dynamic_cast<MixBridgeEditor*>(getActiveEditor()))
            juce::MessageManager::callAsync([editor] { editor->refreshReceiverList(); });
    };

    cb.onReceiverListUpdated = [this](const std::vector<ReceiverInfo>&) {
        if (auto* editor = dynamic_cast<MixBridgeEditor*>(getActiveEditor()))
            juce::MessageManager::callAsync([editor] { editor->refreshReceiverList(); });
    };

    discovery_.start(streamer_.boundPort(), cb);
}

//==============================================================================
// prepareToPlay
//==============================================================================

void MixBridgeProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate_ = sampleRate;
    currentBlockSize_  = samplesPerBlock;

    streamer_.setAudioParameters(sampleRate, samplesPerBlock);

    // Reset ring buffer only when not actively streaming (avoid mid-session flush)
    if (!streamer_.isStreaming()) {
        ringBuffer_.reset();
    }

    // Reset meters
    holdPeakL_    = 0.0f;
    holdPeakR_    = 0.0f;
    holdCounterL_ = 0;
    holdCounterR_ = 0;
    rawPeakL_.store(0.0f, std::memory_order_relaxed);
    rawPeakR_.store(0.0f, std::memory_order_relaxed);
    displayPeakL_.store(0.0f, std::memory_order_relaxed);
    displayPeakR_.store(0.0f, std::memory_order_relaxed);

    // If streaming, send new session config
    if (streamer_.isStreaming()) {
        // The network thread will detect the parameters change
        // and re-negotiate if needed
    }
}

//==============================================================================
// releaseResources
//==============================================================================

void MixBridgeProcessor::releaseResources()
{
    // Nothing to release - ring buffer persists, network thread keeps running
}

//==============================================================================
// isBusesLayoutSupported
//==============================================================================

bool MixBridgeProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // We require stereo in and stereo out (we're a pass-through analyzer)
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    return true;
}

//==============================================================================
// processBlock - AUDIO THREAD
// This runs in the real-time audio thread.
// ABSOLUTELY NO: mutex locks, allocations, I/O, system calls, logging.
//==============================================================================

void MixBridgeProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                       juce::MidiBuffer& /*midiMessages*/)
{
    juce::ScopedNoDenormals noDenormals;

    const int numChannels = buffer.getNumChannels();
    const int numSamples  = buffer.getNumSamples();

    if (numChannels < 2 || numSamples == 0) return;

    const float* leftData  = buffer.getReadPointer(0);
    const float* rightData = buffer.getReadPointer(1);

    // Update peak meters (always, even when bypassed - for UI feedback)
    updatePeakMeters(buffer);

    // Update signal levels for heartbeat (atomic store, safe on audio thread)
    streamer_.updateSignalLevels(
        rawPeakL_.load(std::memory_order_relaxed),
        rawPeakR_.load(std::memory_order_relaxed));

    // Skip ring buffer write if bypassed or not streaming
    if (bypassStream_.load(std::memory_order_relaxed)) return;
    if (streamer_.state() < SessionState::Streaming)   return;

    // Write to ring buffer - lock-free, safe for audio thread
    // If buffer is full (consumer too slow), we drop frames rather than block
    const size_t written = ringBuffer_.write(leftData, rightData,
                                              static_cast<size_t>(numSamples));

    // We intentionally don't assert written == numSamples here.
    // Frame drops under extreme conditions are acceptable; blocking is not.
    // The receiver's jitter buffer handles occasional gaps.
    (void)written;

    // Audio passes through unmodified - we are a non-destructive insert
}

//==============================================================================
// updatePeakMeters - called from audio thread (no allocations allowed)
//==============================================================================

void MixBridgeProcessor::updatePeakMeters(const juce::AudioBuffer<float>& buffer) noexcept
{
    const int numSamples = buffer.getNumSamples();

    const float peakL = computePeak(buffer.getReadPointer(0), numSamples);
    const float peakR = computePeak(buffer.getReadPointer(1), numSamples);

    rawPeakL_.store(peakL, std::memory_order_relaxed);
    rawPeakR_.store(peakR, std::memory_order_relaxed);

    // Peak hold with decay
    if (peakL >= holdPeakL_) {
        holdPeakL_    = peakL;
        holdCounterL_ = kPeakHoldSamples;
    } else {
        if (holdCounterL_ > 0)
            --holdCounterL_;
        else
            holdPeakL_ *= kDecayPerSample;
    }

    if (peakR >= holdPeakR_) {
        holdPeakR_    = peakR;
        holdCounterR_ = kPeakHoldSamples;
    } else {
        if (holdCounterR_ > 0)
            --holdCounterR_;
        else
            holdPeakR_ *= kDecayPerSample;
    }

    displayPeakL_.store(holdPeakL_, std::memory_order_relaxed);
    displayPeakR_.store(holdPeakR_, std::memory_order_relaxed);
}

//==============================================================================
// AudioProcessorValueTreeState listener
//==============================================================================

void MixBridgeProcessor::parameterValueChanged(int parameterIndex, float newValue)
{
    // Update atomics from parameter changes (safe to call from any thread)
    const auto* param = parameters_.getParameter(Params::kBypassStream);
    if (param && parameters_.getParameter(Params::kBypassStream)->getParameterIndex()
        == parameterIndex) {
        bypassStream_.store(newValue > 0.5f, std::memory_order_relaxed);
    }

    const auto* autoParam = parameters_.getParameter(Params::kAutoConnect);
    if (autoParam && autoParam->getParameterIndex() == parameterIndex) {
        autoConnect_.store(newValue > 0.5f, std::memory_order_relaxed);
    }
}

void MixBridgeProcessor::parameterGestureChanged(int, bool) {}

//==============================================================================
// State persistence
//==============================================================================

void MixBridgeProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = parameters_.copyState();

    // Also save last connected device info
    juce::ValueTree extra("MixBridgeExtra");
    {
        std::lock_guard<std::mutex> lock(deviceNameMutex_);
        extra.setProperty("lastDevice", juce::String(connectedDeviceName_), nullptr);
    }
    state.addChild(extra, -1, nullptr);

    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void MixBridgeProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (!xmlState) return;

    auto state = juce::ValueTree::fromXml(*xmlState);
    if (!state.isValid()) return;

    parameters_.replaceState(state);

    // Restore bypass flag from parameter
    if (auto* p = parameters_.getParameter(Params::kBypassStream))
        bypassStream_.store(p->getValue() > 0.5f, std::memory_order_relaxed);

    if (auto* p = parameters_.getParameter(Params::kAutoConnect))
        autoConnect_.store(p->getValue() > 0.5f, std::memory_order_relaxed);
}

//==============================================================================
// Editor
//==============================================================================

juce::AudioProcessorEditor* MixBridgeProcessor::createEditor()
{
    return new MixBridgeEditor(*this);
}

} // namespace MixBridge

//==============================================================================
// JUCE plugin factory entry point
//==============================================================================

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MixBridge::MixBridgeProcessor();
}
