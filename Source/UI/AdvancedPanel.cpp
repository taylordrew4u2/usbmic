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

    closeButton.onClick = [this] { if (onCloseClicked) onCloseClicked(); };
    addAndMakeVisible (closeButton);

    micSelectionLabel.setText ("Microphones to record", juce::dontSendNotification);
    addAndMakeVisible (micSelectionLabel);

    storageLabel.setText ("Save recordings to", juce::dontSendNotification);
    addAndMakeVisible (storageLabel);

    storageCombo.onChange = [this] {
        const int index = storageCombo.getSelectedItemIndex();

        if (index >= 0 && index < storagePaths.size() && onStorageVolumeChosen)
            onStorageVolumeChosen (storagePaths[index]);
    };
    addAndMakeVisible (storageCombo);

    // §10.3 says nothing behind this door may be a gate the novice must pass,
    // which makes an unexplained term worse than no term: someone who does not
    // know what a clock master is cannot tell whether they need to care.
    clockMasterHelpLabel.setText (
        "Clock master: every USB microphone runs on its own crystal, and no two "
        "tick at exactly the same rate. One is chosen as the reference and the "
        "others are continuously nudged to match it, which is what keeps the "
        "tracks lined up over a long take. Leave this alone unless one mic drifts "
        "much more than the rest -- the app picks a sensible one for you.",
        juce::dontSendNotification);
    clockMasterHelpLabel.setJustificationType (juce::Justification::topLeft);
    clockMasterHelpLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (clockMasterHelpLabel);

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

void AdvancedPanel::setMicSelections (const std::vector<std::pair<juce::String, bool>>& mics)
{
    // Rebuilt only when the set of names changes. The panel repaints at 2 Hz,
    // and recreating the toggles every tick would fight the user's click.
    juce::StringArray incoming;
    for (const auto& m : mics)
        incoming.add (m.first);

    if (incoming != lastMicNames)
    {
        micToggles.clear();
        lastMicNames = incoming;

        for (const auto& m : mics)
        {
            auto toggle = std::make_unique<juce::ToggleButton> (m.first);
            const auto name = m.first;

            // Deferred rather than called straight through. Ticking a box
            // rebuilds the audio streams, and a rebuild can reach back into
            // this panel; destroying a button from inside its own click
            // handler is a crash JUCE gives no warning about. Reading the
            // state here and doing the work on the next message keeps the
            // button alive for the whole of its own callback.
            toggle->onClick = [this, name, raw = toggle.get()] {
                const bool state = raw->getToggleState();

                // The SafePointer is built here and captured by copy, rather
                // than constructed in the inner lambda's init-capture. Inside a
                // nested lambda MSVC resolves `this` to the enclosing closure
                // object instead of the panel, so the init-capture form
                // compiled on Clang and GCC and failed on Windows.
                juce::Component::SafePointer<AdvancedPanel> safe (this);

                juce::MessageManager::callAsync ([safe, name, state] {
                    if (safe != nullptr && safe->onMicEnabledChanged)
                        safe->onMicEnabledChanged (name, state);
                });
            };
            addAndMakeVisible (*toggle);
            micToggles.push_back (std::move (toggle));
        }

        resized();
    }

    // State is refreshed every tick regardless, so a change made elsewhere --
    // the 8-mic cap, a device leaving -- shows up here.
    for (size_t i = 0; i < micToggles.size() && i < mics.size(); ++i)
        micToggles[i]->setToggleState (mics[i].second, juce::dontSendNotification);
}

void AdvancedPanel::setStorageVolumes (const std::vector<VolumeChoice>& volumes)
{
    juce::StringArray labels;
    for (const auto& v : volumes)
        labels.add (v.label);

    // Rebuilt only when the set of volumes changes -- a card being plugged in
    // or pulled out. Repopulating on every tick would close the menu under
    // someone in the middle of choosing from it.
    if (labels != lastStorageLabels)
    {
        lastStorageLabels = labels;
        storagePaths.clear();
        storageCombo.clear (juce::dontSendNotification);

        int id = 1;
        for (const auto& v : volumes)
        {
            storageCombo.addItem (v.label, id++);
            storagePaths.add (v.path);
        }
    }

    for (size_t i = 0; i < volumes.size(); ++i)
        if (volumes[i].current)
            storageCombo.setSelectedItemIndex (static_cast<int> (i), juce::dontSendNotification);
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

int AdvancedPanel::getRequiredHeight() const
{
    // Mirrors resized() below. Kept as an explicit sum rather than measured
    // from a trial layout, because resized() consumes the bounds it is given
    // and cannot report what it would have wanted from a taller one.
    constexpr int kMargins       = 12 * 2;
    constexpr int kCloseButton   = 30 + 8;
    constexpr int kRow           = 26 + 4;
    constexpr int kMicListLabel  = 22;
    constexpr int kMicToggle     = 24 + 2;
    constexpr int kClockHelp     = 76 + 6;
    constexpr int kDrift         = 60 + 4;
    constexpr int kTrimViewport  = 100 + 4;
    constexpr int kAggregate     = 20 + 4;
    constexpr int kMirror        = 26 + 4;
    constexpr int kDestination   = 26 + 8;
    constexpr int kDiagnostics   = 30;

    // sample rate, bit depth, buffer size, latency, clock master, output
    // device, backend, aggregate name, save-to volume.
    constexpr int kRowCount = 9;

    return kMargins + kCloseButton + (kRow * kRowCount) + kMicListLabel
         + static_cast<int> (micToggles.size()) * kMicToggle + 8
         + kClockHelp + kDrift + kTrimViewport + kAggregate + kMirror
         + kDestination + kDiagnostics;
}

void AdvancedPanel::resized()
{
    auto area = getLocalBounds().reduced (12);

    // Top-left and first in the layout, where a back control is looked for,
    // and placed before anything else claims the space so it cannot be pushed
    // off the bottom by a long device list.
    closeButton.setBounds (area.removeFromTop (30).removeFromLeft (110));
    area.removeFromTop (8);

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
    micSelectionLabel.setBounds (area.removeFromTop (22));
    for (auto& toggle : micToggles)
    {
        toggle->setBounds (area.removeFromTop (24).reduced (8, 0));
        area.removeFromTop (2);
    }
    area.removeFromTop (8);

    row (clockMasterLabel, clockMasterCombo);
    clockMasterHelpLabel.setBounds (area.removeFromTop (76));
    area.removeFromTop (6);

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

    row (storageLabel, storageCombo);

    {
        auto r = area.removeFromTop (26);
        destinationFolderLabel.setBounds (r.removeFromLeft (r.getWidth() / 2));
        destinationFolderButton.setBounds (r);
    }
    area.removeFromTop (8);

    diagnosticsExportButton.setBounds (area.removeFromTop (30));
}

} // namespace mma
