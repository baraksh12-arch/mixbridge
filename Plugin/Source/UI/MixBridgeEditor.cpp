/*
  ==============================================================================
  MixBridgeEditor.cpp
  Full VST3 plugin UI implementation.
  ==============================================================================
*/

#include "MixBridgeEditor.h"
#include <cmath>

namespace MixBridge {

//==============================================================================
// SignalMeter
//==============================================================================

SignalMeter::SignalMeter()
{
    startTimerHz(30);
}

void SignalMeter::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    // Background
    g.setColour(juce::Colour(0xFF111114));
    g.fillRoundedRectangle(bounds, 4.0f);

    // Smoothing
    const float smoothFactor = 0.7f;
    displayL_ = displayL_ * smoothFactor + targetL_ * (1.0f - smoothFactor);
    displayR_ = displayR_ * smoothFactor + targetR_ * (1.0f - smoothFactor);

    const float channelW = (bounds.getWidth() - 6.0f) / 2.0f;
    const float maxH     = bounds.getHeight() - 8.0f;

    auto drawChannel = [&](float linear, float x)
    {
        const float dB      = linearToDB(linear);
        const float clamped = juce::jlimit(-60.0f, 0.0f, dB);
        const float ratio   = (clamped + 60.0f) / 60.0f;
        const float barH    = maxH * ratio;

        // Background track
        g.setColour(juce::Colour(0xFF2A2A30));
        g.fillRect(x, bounds.getY() + 4.0f, channelW, maxH);

        if (barH > 0.0f) {
            // Gradient bar: green → amber → red
            juce::ColourGradient grad(
                juce::Colour(0xFF30D158), x, bounds.getBottom() - 4.0f,
                juce::Colour(0xFFFF3B30), x, bounds.getY() + 4.0f,
                false);
            grad.addColour(0.75, juce::Colour(0xFFFF9500));

            g.setGradientFill(grad);
            g.fillRect(x, bounds.getBottom() - 4.0f - barH, channelW, barH);
        }

        // dB labels on first channel
        if (x < 10.0f) {
            g.setColour(juce::Colour(0xFF555566));
            g.setFont(9.0f);
            for (float db : {0.0f, -6.0f, -12.0f, -24.0f, -48.0f}) {
                const float ratio2 = (db + 60.0f) / 60.0f;
                const float y = bounds.getBottom() - 4.0f - maxH * ratio2;
                g.drawHorizontalLine(static_cast<int>(y),
                    bounds.getX(), bounds.getRight());
            }
        }
    };

    drawChannel(displayL_, bounds.getX() + 4.0f);
    drawChannel(displayR_, bounds.getX() + 4.0f + channelW + 2.0f);

    // Border
    g.setColour(juce::Colour(0xFF444455));
    g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.0f);
}

//==============================================================================
// StatusDot
//==============================================================================

StatusDot::StatusDot()
{
    startTimerHz(2);
}

void StatusDot::paint(juce::Graphics& g)
{
    juce::Colour c;
    bool shouldBlink = false;

    switch (state_) {
        case SessionState::Idle:
            c = juce::Colour(0xFF555566);
            break;
        case SessionState::Searching:
        case SessionState::Connecting:
        case SessionState::Configuring:
        case SessionState::Reconnecting:
            c = juce::Colour(0xFFFF9500);
            shouldBlink = true;
            break;
        case SessionState::Streaming:
            c = juce::Colour(0xFF30D158);
            break;
    }

    const float alpha = (shouldBlink && !blinkPhase_) ? 0.3f : 1.0f;
    g.setColour(c.withAlpha(alpha));

    const auto bounds = getLocalBounds().toFloat().reduced(2.0f);
    g.fillEllipse(bounds);

    // Glow for streaming state
    if (state_ == SessionState::Streaming) {
        g.setColour(c.withAlpha(0.25f));
        g.fillEllipse(bounds.expanded(3.0f));
    }
}

//==============================================================================
// ReceiverListBox
//==============================================================================

ReceiverListBox::ReceiverListBox(MixBridgeProcessor& proc)
    : processor_(proc)
    , listBox_("Receivers", this)
{
    listBox_.setColour(juce::ListBox::backgroundColourId,
                       juce::Colour(0xFF1E1E24));
    listBox_.setColour(juce::ListBox::outlineColourId,
                       juce::Colour(0xFF444455));
    listBox_.setRowHeight(36);
    addAndMakeVisible(listBox_);
}

void ReceiverListBox::refresh()
{
    receivers_ = processor_.getDiscoveredReceivers();
    listBox_.updateContent();
}

void ReceiverListBox::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xFF1E1E24));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    g.setColour(juce::Colour(0xFF444455));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 6.0f, 1.0f);
}

void ReceiverListBox::resized()
{
    listBox_.setBounds(getLocalBounds().reduced(1));
}

int ReceiverListBox::getNumRows()
{
    return static_cast<int>(receivers_.size());
}

void ReceiverListBox::paintListBoxItem(int row, juce::Graphics& g,
                                        int w, int h, bool selected)
{
    if (row < 0 || row >= static_cast<int>(receivers_.size())) return;
    const auto& r = receivers_[static_cast<size_t>(row)];

    if (selected) {
        g.setColour(juce::Colour(0xFF0A84FF).withAlpha(0.3f));
        g.fillAll();
    }

    // Device icon
    g.setColour(juce::Colour(0xFF0A84FF));
    g.fillRoundedRectangle(12.0f, (h - 20) / 2.0f, 14.0f, 20.0f, 3.0f);

    // Name
    g.setColour(juce::Colour(0xFFEEEEEE));
    g.setFont(juce::Font(14.0f, juce::Font::bold));
    g.drawText(juce::String(r.name), 34, 4, w - 80, h / 2 - 2,
               juce::Justification::centredLeft);

    // IP address
    g.setColour(juce::Colour(0xFF888899));
    g.setFont(11.0f);
    g.drawText(juce::String(r.address) + ":" + juce::String(r.port),
               34, h / 2, w - 80, h / 2 - 4,
               juce::Justification::centredLeft);

    // "Tap to connect" hint
    g.setColour(juce::Colour(0xFF555566));
    g.setFont(10.0f);
    g.drawText("double-click", w - 76, 0, 70, h,
               juce::Justification::centredRight);
}

void ReceiverListBox::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    if (row < 0 || row >= static_cast<int>(receivers_.size())) return;
    processor_.connectToReceiver(receivers_[static_cast<size_t>(row)]);
}

//==============================================================================
// StatLabel
//==============================================================================

StatLabel::StatLabel(const juce::String& labelText)
{
    nameLabel_.setText(labelText, juce::dontSendNotification);
    nameLabel_.setFont(juce::Font(10.0f));
    nameLabel_.setColour(juce::Label::textColourId, juce::Colour(0xFF888899));
    nameLabel_.setJustificationType(juce::Justification::centred);

    valueLabel_.setText("--", juce::dontSendNotification);
    valueLabel_.setFont(juce::Font(13.0f, juce::Font::bold));
    valueLabel_.setColour(juce::Label::textColourId, juce::Colour(0xFFEEEEEE));
    valueLabel_.setJustificationType(juce::Justification::centred);

    addAndMakeVisible(nameLabel_);
    addAndMakeVisible(valueLabel_);
}

void StatLabel::resized()
{
    const auto b = getLocalBounds();
    valueLabel_.setBounds(b.withHeight(b.getHeight() / 2).withY(2));
    nameLabel_.setBounds(b.withHeight(b.getHeight() / 2).withY(b.getHeight() / 2));
}

//==============================================================================
// MixBridgeEditor
//==============================================================================

MixBridgeEditor::MixBridgeEditor(MixBridgeProcessor& processor)
    : AudioProcessorEditor(processor)
    , processor_(processor)
    , latencyStat_("LATENCY")
    , lossStat_("PACKET LOSS")
    , sampleRateStat_("SAMPLE RATE")
    , qualityStat_("NETWORK")
    , receiverList_(processor)
{
    setSize(kWidth, kHeight);
    setResizable(false, false);

    // Title
    titleLabel_.setText("MIXBRIDGE", juce::dontSendNotification);
    titleLabel_.setFont(juce::Font(18.0f, juce::Font::bold));
    titleLabel_.setColour(juce::Label::textColourId, juce::Colour(kTextColour));
    addAndMakeVisible(titleLabel_);

    // Status dot + label
    addAndMakeVisible(statusDot_);

    statusLabel_.setFont(juce::Font(13.0f));
    statusLabel_.setColour(juce::Label::textColourId, juce::Colour(kDimTextColour));
    addAndMakeVisible(statusLabel_);

    deviceLabel_.setFont(juce::Font(13.0f, juce::Font::bold));
    deviceLabel_.setColour(juce::Label::textColourId, juce::Colour(kAccentColour));
    addAndMakeVisible(deviceLabel_);

    // Meters
    addAndMakeVisible(meterDisplay_);
    meterLabelL_.setText("L", juce::dontSendNotification);
    meterLabelR_.setText("R", juce::dontSendNotification);
    for (auto* l : {&meterLabelL_, &meterLabelR_}) {
        l->setFont(juce::Font(10.0f, juce::Font::bold));
        l->setColour(juce::Label::textColourId, juce::Colour(kDimTextColour));
        l->setJustificationType(juce::Justification::centred);
        addAndMakeVisible(l);
    }

    // Stats
    addAndMakeVisible(latencyStat_);
    addAndMakeVisible(lossStat_);
    addAndMakeVisible(sampleRateStat_);
    addAndMakeVisible(qualityStat_);

    // Receivers
    receiversTitle_.setText("AVAILABLE RECEIVERS", juce::dontSendNotification);
    receiversTitle_.setFont(juce::Font(10.0f, juce::Font::bold));
    receiversTitle_.setColour(juce::Label::textColourId, juce::Colour(kDimTextColour));
    addAndMakeVisible(receiversTitle_);
    addAndMakeVisible(receiverList_);

    // Buttons
    reconnectButton_.setButtonText("Reconnect");
    reconnectButton_.setColour(juce::TextButton::buttonColourId,
                                juce::Colour(kAccentColour));
    reconnectButton_.setColour(juce::TextButton::textColourOffId,
                                juce::Colours::white);
    reconnectButton_.onClick = [this] { processor_.reconnect(); };
    addAndMakeVisible(reconnectButton_);

    disconnectButton_.setButtonText("Disconnect");
    disconnectButton_.setColour(juce::TextButton::buttonColourId,
                                 juce::Colour(0xFF3A3A45));
    disconnectButton_.setColour(juce::TextButton::textColourOffId,
                                 juce::Colour(kTextColour));
    disconnectButton_.onClick = [this] { processor_.disconnect(); };
    addAndMakeVisible(disconnectButton_);

    // Latency mode
    latencyModeLabel_.setText("Latency Mode", juce::dontSendNotification);
    latencyModeLabel_.setFont(juce::Font(11.0f));
    latencyModeLabel_.setColour(juce::Label::textColourId,
                                 juce::Colour(kDimTextColour));
    addAndMakeVisible(latencyModeLabel_);

    latencyModeCombo_.addItem("Auto",      1);
    latencyModeCombo_.addItem("Ultra Low", 2);
    latencyModeCombo_.addItem("Low",       3);
    latencyModeCombo_.addItem("Normal",    4);
    latencyModeCombo_.addItem("Stable",    5);
    latencyModeCombo_.setColour(juce::ComboBox::backgroundColourId,
                                 juce::Colour(0xFF2A2A32));
    latencyModeCombo_.setColour(juce::ComboBox::textColourId,
                                 juce::Colour(kTextColour));
    latencyModeCombo_.setColour(juce::ComboBox::outlineColourId,
                                 juce::Colour(0xFF444455));
    addAndMakeVisible(latencyModeCombo_);

    latencyModeAttach_ = std::make_unique<
        juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            processor_.getParameters(), Params::kLatencyMode, latencyModeCombo_);

    // Auto-connect toggle
    autoConnectToggle_.setButtonText("Auto-connect");
    autoConnectToggle_.setColour(juce::ToggleButton::textColourId,
                                  juce::Colour(kDimTextColour));
    autoConnectToggle_.setColour(juce::ToggleButton::tickColourId,
                                  juce::Colour(kAccentColour));
    addAndMakeVisible(autoConnectToggle_);

    autoConnectAttach_ = std::make_unique<
        juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor_.getParameters(), Params::kAutoConnect, autoConnectToggle_);

    // Bypass toggle
    bypassToggle_.setButtonText("Bypass");
    bypassToggle_.setColour(juce::ToggleButton::textColourId,
                             juce::Colour(kDimTextColour));
    bypassToggle_.setColour(juce::ToggleButton::tickColourId,
                             juce::Colour(kAmberColour));
    addAndMakeVisible(bypassToggle_);

    bypassAttach_ = std::make_unique<
        juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processor_.getParameters(), Params::kBypassStream, bypassToggle_);

    // Timer for real-time updates
    startTimerHz(20);

    // Initial refresh
    refreshState();
    refreshReceiverList();
}

MixBridgeEditor::~MixBridgeEditor()
{
    stopTimer();
}

void MixBridgeEditor::paint(juce::Graphics& g)
{
    // Dark background
    g.fillAll(juce::Colour(kBgColour));

    // Header panel
    g.setColour(juce::Colour(kPanelColour));
    g.fillRoundedRectangle(0.0f, 0.0f, getWidth(), 70.0f, 0.0f);

    // Dividers
    g.setColour(juce::Colour(0xFF333340));
    g.fillRect(0, 70, getWidth(), 1);
    g.fillRect(0, 155, getWidth(), 1);
    g.fillRect(0, 220, getWidth(), 1);

    // Meter panel background
    g.setColour(juce::Colour(kPanelColour));
    g.fillRect(0, 71, getWidth(), 84);

    // Stats panel
    g.setColour(juce::Colour(kPanelColour));
    g.fillRect(0, 221, getWidth(), 54);

    // Version watermark
    g.setColour(juce::Colour(0xFF333340));
    g.setFont(9.0f);
    g.drawText("v1.0", getWidth() - 30, getHeight() - 14, 24, 12,
               juce::Justification::centredRight);
}

void MixBridgeEditor::resized()
{
    layoutComponents();
}

void MixBridgeEditor::layoutComponents()
{
    const int margin = 12;
    const int w      = getWidth() - margin * 2;

    // Header
    titleLabel_.setBounds(margin, 12, 160, 24);
    statusDot_.setBounds(getWidth() - margin - 12, 18, 12, 12);
    statusLabel_.setBounds(margin, 38, w, 18);
    deviceLabel_.setBounds(margin + 60, 38, w - 60, 18);

    // Meters
    const int meterY = 78;
    meterLabelL_.setBounds(margin, meterY, 14, 70);
    meterDisplay_.setBounds(margin + 16, meterY, w - 54, 70);
    meterLabelR_.setBounds(margin + 16 + w - 54 + 4, meterY, 14, 70);

    // Stats row
    const int statsY   = 225;
    const int statW    = (w - margin * 3) / 4;
    for (int i = 0; i < 4; ++i) {
        const int x = margin + i * (statW + margin / 2);
        switch (i) {
            case 0: latencyStat_.setBounds(x, statsY, statW, 42);    break;
            case 1: lossStat_.setBounds(x, statsY, statW, 42);       break;
            case 2: sampleRateStat_.setBounds(x, statsY, statW, 42); break;
            case 3: qualityStat_.setBounds(x, statsY, statW, 42);    break;
        }
    }

    // Receivers
    receiversTitle_.setBounds(margin, 275, w, 16);
    receiverList_.setBounds(margin, 293, w, 100);

    // Latency mode
    latencyModeLabel_.setBounds(margin, 403, 90, 22);
    latencyModeCombo_.setBounds(margin + 94, 403, 120, 22);

    // Toggles
    autoConnectToggle_.setBounds(margin, 432, 110, 20);
    bypassToggle_.setBounds(margin + 120, 432, 80, 20);

    // Buttons
    const int btnW = (w - margin) / 2;
    reconnectButton_.setBounds(margin, 460, btnW, 28);
    disconnectButton_.setBounds(margin + btnW + margin, 460, btnW, 28);
}

void MixBridgeEditor::timerCallback()
{
    updateStats();
}

void MixBridgeEditor::refreshState()
{
    const SessionState state = processor_.getSessionState();
    statusDot_.setState(state);
    statusLabel_.setText(getStateString(state), juce::dontSendNotification);

    const std::string device = processor_.getConnectedDeviceName();
    deviceLabel_.setText(device.empty() ? "" : juce::String(device),
                         juce::dontSendNotification);
}

void MixBridgeEditor::refreshReceiverList()
{
    receiverList_.refresh();
}

void MixBridgeEditor::updateStats()
{
    // Update meters
    meterDisplay_.setLevels(
        processor_.getSignalPeakL(),
        processor_.getSignalPeakR());

    // Update stats labels
    const float latency = processor_.getCurrentLatencyMs();
    latencyStat_.setValue(latency > 0.0f ? formatLatency(latency) : "--");

    const auto snap = processor_.getNetworkStats();
    lossStat_.setValue(snap.packetsSent > 0 ? formatLoss(snap.lossPercent) : "--");

    const double sr = processor_.getSampleRate();
    if (sr > 0)
        sampleRateStat_.setValue(
            juce::String(static_cast<int>(sr / 1000.0)) + "k");
    else
        sampleRateStat_.setValue("--");

    using Q = Protocol::NetworkQuality;
    juce::String qualStr;
    juce::Colour qualColour = juce::Colour(kDimTextColour);

    switch (snap.quality) {
        case Q::Excellent: qualStr = "Excellent"; qualColour = juce::Colour(kGreenColour);  break;
        case Q::Good:      qualStr = "Good";      qualColour = juce::Colour(kGreenColour);  break;
        case Q::Fair:      qualStr = "Fair";      qualColour = juce::Colour(kAmberColour);  break;
        case Q::Poor:      qualStr = "Poor";      qualColour = juce::Colour(kRedColour);    break;
        default:           qualStr = "--";         break;
    }
    qualityStat_.setValue(qualStr);
}

juce::String MixBridgeEditor::formatLatency(float ms) const
{
    if (ms < 1.0f)
        return juce::String(static_cast<int>(ms * 1000.0f)) + " μs";
    return juce::String(ms, 1) + " ms";
}

juce::String MixBridgeEditor::formatLoss(float pct) const
{
    if (pct < 0.01f) return "0%";
    return juce::String(pct, 1) + "%";
}

juce::String MixBridgeEditor::getStateString(SessionState state) const
{
    switch (state) {
        case SessionState::Idle:         return "Not connected";
        case SessionState::Searching:    return "Searching...";
        case SessionState::Connecting:   return "Connecting...";
        case SessionState::Configuring:  return "Configuring...";
        case SessionState::Streaming:    return "Streaming to:";
        case SessionState::Reconnecting: return "Reconnecting...";
    }
    return {};
}

} // namespace MixBridge
