#include "AdvancedPanel.h"

namespace mma {

namespace {
void configureRow (juce::Label& label, juce::Label& value, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
}
} // namespace

AdvancedPanel::AdvancedPanel()
{
    configureRow (sampleRateLabel, sampleRateValue, "Sample rate");
    configureRow (bitDepthLabel, bitDepthValue, "Bit depth");
    configureRow (bufferSizeLabel, bufferSizeValue, "Buffer size");
    configureRow (latencyLabel, latencyValue, "Measured latency");
    clockMasterLabel.setText ("Clock master", juce::dontSendNotification);
    driftLabel.setText ("Per-device drift (PPM)", juce::dontSendNotification);
    outputDeviceLabel.setText ("Output device", juce::dontSendNotification);
    backendLabel.setText ("Virtual device backend", juce::dontSendNotification);
    destinationFolderLabel.setText ("Destination folder", juce::dontSendNotification);

    for (auto* c : { &sampleRateLabel, &sampleRateValue, &bitDepthLabel, &bitDepthValue,
                     &bufferSizeLabel, &bufferSizeValue, &latencyLabel, &latencyValue,
                     &clockMasterLabel, &driftLabel, &outputDeviceLabel, &backendLabel,
                     &backendValue, &destinationFolderLabel })
        addAndMakeVisible (c);

    addAndMakeVisible (clockMasterCombo);
    clockMasterCombo.onChange = [this] {
        if (onClockMasterChanged)
            onClockMasterChanged (clockMasterCombo.getText());
    };

    addAndMakeVisible (outputDeviceCombo);
    outputDeviceCombo.onChange = [this] {
        if (onOutputDeviceChanged)
            onOutputDeviceChanged (outputDeviceCombo.getText());
    };

    trimViewport.setViewedComponent (&trimContainer, false);
    addAndMakeVisible (trimViewport);

    mirrorToggle.setToggleState (true, juce::dontSendNotification); // §6.3 default on
    mirrorToggle.onClick = [this] { if (onMirrorToggled) onMirrorToggled (mirrorToggle.getToggleState()); };
    addAndMakeVisible (mirrorToggle);

    destinationFolderButton.onClick = [this] { if (onDestinationFolderClicked) onDestinationFolderClicked(); };
    addAndMakeVisible (destinationFolderButton);

    diagnosticsExportButton.onClick = [this] { if (onDiagnosticsExportClicked) onDiagnosticsExportClicked(); };
    addAndMakeVisible (diagnosticsExportButton);
}

AdvancedPanel::~AdvancedPanel() = default;

void AdvancedPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xFF1E1816));
}

void AdvancedPanel::resized()
{
    auto area = getLocalBounds().reduced (12);
    auto row = [&] (juce::Label& label, juce::Component& value) {
        auto r = area.removeFromTop (26);
        label.setBounds (r.removeFromLeft (r.getWidth() / 2));
        value.setBounds (r);
        area.removeFromTop (4);
    };

    row (sampleRateLabel, sampleRateValue);
    row (bitDepthLabel, bitDepthValue);
    row (bufferSizeLabel, bufferSizeValue);
    row (latencyLabel, latencyValue);
    row (clockMasterLabel, clockMasterCombo);

    driftLabel.setBounds (area.removeFromTop (60));
    area.removeFromTop (4);

    trimViewport.setBounds (area.removeFromTop (100));
    area.removeFromTop (4);

    row (outputDeviceLabel, outputDeviceCombo);
    row (backendLabel, backendValue);

    mirrorToggle.setBounds (area.removeFromTop (26));
    area.removeFromTop (4);

    {
        auto r = area.removeFromTop (26);
        destinationFolderLabel.setBounds (r.removeFromLeft (r.getWidth() / 2));
        destinationFolderButton.setBounds (r);
    }
    area.removeFromTop (8);

    diagnosticsExportButton.setBounds (area.removeFromTop (30));
}

} // namespace mma
