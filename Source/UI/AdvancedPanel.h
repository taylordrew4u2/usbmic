#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace mma {

/// §10.3: the Advanced panel's exhaustive contents. One door, one click,
/// nothing behind it required for correct operation -- everything here is
/// informational/optional, never a gate the novice must pass through.
class AdvancedPanel : public juce::Component
{
public:
    AdvancedPanel();
    ~AdvancedPanel() override;

    void resized() override;
    void paint (juce::Graphics& g) override;

    void setSampleRate (double rate) { sampleRateValue.setText (juce::String (rate, 0) + " Hz", juce::dontSendNotification); }
    void setBitDepth (int bits) { bitDepthValue.setText (juce::String (bits) + "-bit", juce::dontSendNotification); }
    void setBufferSize (int samples) { bufferSizeValue.setText (juce::String (samples) + " samples", juce::dontSendNotification); }
    void setMeasuredLatency (double ms) { latencyValue.setText (juce::String (ms, 1) + " ms", juce::dontSendNotification); }
    void setActiveBackendDescription (const juce::String& text) { backendValue.setText (text, juce::dontSendNotification); }

    std::function<void()> onDiagnosticsExportClicked;
    std::function<void (bool)> onMirrorToggled;
    std::function<void()> onDestinationFolderClicked;
    std::function<void (const juce::String&)> onClockMasterChanged;
    std::function<void (const juce::String&)> onOutputDeviceChanged;

private:
    juce::Label sampleRateLabel, sampleRateValue;
    juce::Label bitDepthLabel, bitDepthValue;
    juce::Label bufferSizeLabel, bufferSizeValue;
    juce::Label latencyLabel, latencyValue;
    juce::Label clockMasterLabel;
    juce::ComboBox clockMasterCombo;
    juce::Label driftLabel; // per-device drift in PPM, populated externally as a multi-line label
    juce::Viewport trimViewport; // per-microphone trim sliders, one row per device
    juce::Component trimContainer;
    juce::Label outputDeviceLabel;
    juce::ComboBox outputDeviceCombo;
    juce::Label backendLabel, backendValue;
    juce::ToggleButton mirrorToggle { "Keep a local backup copy" };
    juce::Label destinationFolderLabel;
    juce::TextButton destinationFolderButton { "Change..." };
    juce::TextButton diagnosticsExportButton { "Export diagnostics" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdvancedPanel)
};

} // namespace mma
