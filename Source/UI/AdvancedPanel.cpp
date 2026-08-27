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
    driftLabel.setJustificationType (juce::Justification::topLeft);
    outputDeviceLabel.setText ("Output device", juce::dontSendNotification);
    backendLabel.setText ("Virtual device backend", juce::dontSendNotification);
    aggregateNameLabel.setText ("Combined device name", juce::dontSendNotification);
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

    // The name other apps see this rig under. Applied when typing finishes,
    // not per keystroke -- each change replaces a device other apps may be
    // recording from.
    addAndMakeVisible (aggregateNameLabel);
    aggregateNameEditor.setTextToShowWhenEmpty ("Multi-Mic Aggregator", juce::Colours::grey);
    aggregateNameEditor.onReturnKey = [this] { if (onAggregateNameChanged) onAggregateNameChanged (aggregateNameEditor.getText()); };
    aggregateNameEditor.onFocusLost = [this] { if (onAggregateNameChanged) onAggregateNameChanged (aggregateNameEditor.getText()); };
    addAndMakeVisible (aggregateNameEditor);
    aggregateStatusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (aggregateStatusLabel);
}

AdvancedPanel::~AdvancedPanel() = default;

namespace {
void fillCombo (juce::ComboBox& combo, const juce::StringArray& names, const juce::String& selected)
{
    combo.clear (juce::dontSendNotification);

    for (int i = 0; i < names.size(); ++i)
        combo.addItem (names[i], i + 1);

    const auto index = names.indexOf (selected);

    // dontSendNotification throughout: repopulating the list must not look like
    // the user having chosen something.
    if (index >= 0)
        combo.setSelectedId (index + 1, juce::dontSendNotification);
}
} // namespace

void AdvancedPanel::setOutputDevices (const juce::StringArray& names, const juce::String& selected)
{
    fillCombo (outputDeviceCombo, names, selected);
}

void AdvancedPanel::setClockMasters (const juce::StringArray& names, const juce::String& selected)
{
    fillCombo (clockMasterCombo, names, selected);
}

void AdvancedPanel::setTrimChannels (const juce::StringArray& micNames,
                                     const std::function<float (int)>& currentTrimDb)
{
    trimNameLabels.clear();
    trimSliders.clear();

    for (int i = 0; i < micNames.size(); ++i)
    {
        auto label = std::make_unique<juce::Label>();
        label->setText (micNames[i], juce::dontSendNotification);
        trimContainer.addAndMakeVisible (*label);
        trimNameLabels.push_back (std::move (label));

        auto slider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal,
                                                      juce::Slider::TextBoxRight);
        // §4: -20..+20 dB in 0.5 dB steps, defaulting to 0.
        slider->setRange (-20.0, 20.0, 0.5);
        slider->setTextValueSuffix (" dB");
        slider->setValue (currentTrimDb != nullptr ? currentTrimDb (i) : 0.0,
                          juce::dontSendNotification);

        auto* raw = slider.get();
        slider->onValueChange = [this, raw, i] {
            if (onTrimChanged)
                onTrimChanged (i, static_cast<float> (raw->getValue()));
        };

        trimContainer.addAndMakeVisible (*slider);
        trimSliders.push_back (std::move (slider));
    }

    layOutTrimRows();
}

void AdvancedPanel::layOutTrimRows()
{
    constexpr int rowHeight = 26;
    const int width = juce::jmax (200, trimViewport.getWidth() - trimViewport.getScrollBarThickness());

    trimContainer.setSize (width, rowHeight * static_cast<int> (trimSliders.size()));

    for (size_t i = 0; i < trimSliders.size(); ++i)
    {
        juce::Rectangle<int> row (0, rowHeight * static_cast<int> (i), width, rowHeight);
        trimNameLabels[i]->setBounds (row.removeFromLeft (width / 3));
        trimSliders[i]->setBounds (row);
    }
}

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
    layOutTrimRows();
    area.removeFromTop (4);

    row (outputDeviceLabel, outputDeviceCombo);
    row (backendLabel, backendValue);
    row (aggregateNameLabel, aggregateNameEditor);
    aggregateStatusLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (4);

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
