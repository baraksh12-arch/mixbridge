#pragma once
//==============================================================================
// MixBridgeEditor.h
// VST3 plugin UI. Dark professional theme, HiDPI-aware.
// Displays connection status, signal meters, network stats, and receiver list.
//==============================================================================

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Core/MixBridgeProcessor.h"

namespace MixBridge {

//==============================================================================
// SignalMeter - real-time stereo VU/peak meter
//==============================================================================

class SignalMeter final : public juce::Component, public juce::Timer
{
public:
    SignalMeter();
    ~SignalMeter() override { stopTimer(); }

    void setLevels(float leftLinear, float rightLinear) noexcept
    {
        targetL_ = leftLinear;
        targetR_ = rightLinear;
    }

    void paint(juce::Graphics& g) override;
    void timerCallback() override { repaint(); }

private:
    float displayL_ = 0.0f;
    float displayR_ = 0.0f;
    float targetL_  = 0.0f;
    float targetR_  = 0.0f;

    static float linearToDB(float linear) noexcept
    {
        return (linear > 0.0f)
            ? 20.0f * std::log10(linear)
            : -96.0f;
    }

    static juce::Colour meterColour(float dB) noexcept
    {
        if (dB >= -3.0f)  return juce::Colour(0xFFFF3B30); // Red
        if (dB >= -9.0f)  return juce::Colour(0xFFFF9500); // Amber
        return                   juce::Colour(0xFF30D158); // Green
    }
};

//==============================================================================
// StatusDot - animated connection indicator
//==============================================================================

class StatusDot final : public juce::Component, public juce::Timer
{
public:
    StatusDot();
    ~StatusDot() override { stopTimer(); }

    void setState(SessionState state) noexcept
    {
        state_ = state;
        repaint();
    }

    void paint(juce::Graphics& g) override;
    void timerCallback() override
    {
        blinkPhase_ = !blinkPhase_;
        repaint();
    }

private:
    SessionState state_     = SessionState::Idle;
    bool         blinkPhase_= false;
};

//==============================================================================
// ReceiverListBox - shows discovered receivers
//==============================================================================

class ReceiverListBox final : public juce::Component,
                               public juce::ListBoxModel
{
public:
    ReceiverListBox(MixBridgeProcessor& proc);

    void refresh();
    void paint(juce::Graphics& g) override;
    void resized() override;

    // ListBoxModel
    int  getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g,
                          int w, int h, bool selected) override;
    void listBoxItemDoubleClicked(int row,
                                  const juce::MouseEvent&) override;

private:
    MixBridgeProcessor&       processor_;
    juce::ListBox             listBox_;
    std::vector<ReceiverInfo> receivers_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReceiverListBox)
};

//==============================================================================
// StatLabel - compact stat display (label + value)
//==============================================================================

class StatLabel final : public juce::Component
{
public:
    StatLabel(const juce::String& labelText);

    void setValue(const juce::String& value)
    {
        valueLabel_.setText(value, juce::dontSendNotification);
    }

    void resized() override;

private:
    juce::Label nameLabel_;
    juce::Label valueLabel_;
};

//==============================================================================
// MixBridgeEditor - main plugin window
//==============================================================================

class MixBridgeEditor final : public juce::AudioProcessorEditor,
                               public juce::Timer
{
public:
    explicit MixBridgeEditor(MixBridgeProcessor& processor);
    ~MixBridgeEditor() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;

    // Called from processor callbacks (via MessageManager::callAsync)
    void refreshState();
    void refreshReceiverList();

private:
    void layoutComponents();
    void updateStats();
    void styleLabel(juce::Label& label, float fontSize, bool bold = false,
                    juce::Colour colour = juce::Colour(0xFFEEEEEE));
    juce::String formatLatency(float ms) const;
    juce::String formatLoss(float pct) const;
    juce::String getStateString(SessionState state) const;

    MixBridgeProcessor& processor_;

    // ---- Header ----
    juce::Label  titleLabel_;
    StatusDot    statusDot_;
    juce::Label  statusLabel_;
    juce::Label  deviceLabel_;

    // ---- Signal Meters ----
    SignalMeter  meterDisplay_;
    juce::Label  meterLabelL_;
    juce::Label  meterLabelR_;

    // ---- Stats row ----
    StatLabel    latencyStat_;
    StatLabel    lossStat_;
    StatLabel    sampleRateStat_;
    StatLabel    qualityStat_;

    // ---- Receiver list ----
    juce::Label       receiversTitle_;
    ReceiverListBox   receiverList_;

    // ---- Buttons ----
    juce::TextButton  reconnectButton_;
    juce::TextButton  disconnectButton_;

    // ---- Bypass toggle ----
    juce::ToggleButton bypassToggle_;

    // ---- Latency mode combo ----
    juce::Label    latencyModeLabel_;
    juce::ComboBox latencyModeCombo_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> latencyModeAttach_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   bypassAttach_;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   autoConnectAttach_;

    // ---- Auto-connect toggle ----
    juce::ToggleButton autoConnectToggle_;

    // Colour palette
    static constexpr auto kBgColour       = 0xFF1A1A1F;
    static constexpr auto kPanelColour    = 0xFF232328;
    static constexpr auto kAccentColour   = 0xFF0A84FF;
    static constexpr auto kTextColour     = 0xFFEEEEEE;
    static constexpr auto kDimTextColour  = 0xFF888899;
    static constexpr auto kGreenColour    = 0xFF30D158;
    static constexpr auto kRedColour      = 0xFFFF3B30;
    static constexpr auto kAmberColour    = 0xFFFF9500;

    static constexpr int kWidth  = 380;
    static constexpr int kHeight = 520;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixBridgeEditor)
};

} // namespace MixBridge
