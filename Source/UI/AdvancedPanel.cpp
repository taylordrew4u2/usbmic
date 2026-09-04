#include "AdvancedPanel.h"
#include "AppLookAndFeel.h"

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

    // Two tones rather than one. Every row rendered at the same weight, so the
    // question and its answer were indistinguishable down a column.
    for (auto* l : { &sampleRateLabel, &bitDepthLabel, &bufferSizeLabel, &latencyLabel,
                     &clockMasterLabel, &outputDeviceLabel, &backendLabel,
                     &aggregateNameLabel, &destinationFolderLabel, &storageLabel,
                     &micSelectionLabel })
        l->setColour (juce::Label::textColourId, AppLookAndFeel::secondary);

    for (auto* v : { &sampleRateValue, &bitDepthValue, &bufferSizeValue, &latencyValue,
                     &backendValue })
        v->setColour (juce::Label::textColourId, AppLookAndFeel::bone);

    // Supporting text, not body text: the drift report, the aggregate status
    // and the clock-master explanation all sat at the same size and weight as
    // the settings they describe.
    for (auto* l : { &driftLabel, &aggregateStatusLabel, &clockMasterHelpLabel })
    {
        l->setFont (juce::Font (11.0f));
        l->setColour (juce::Label::textColourId, AppLookAndFeel::secondary);
    }

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

    mirrorNote.setText ("in case the first copy gets a little damp", juce::dontSendNotification);
    mirrorNote.setFont (juce::Font (12.0f, juce::Font::italic));
    mirrorNote.setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
    addAndMakeVisible (mirrorNote);

    // Off by default: it costs disk and minutes of CPU after every take, and
    // the picture and the sound are already both saved and already aligned by
    // the session origin. Nobody who does not want it should pay for it.
    combineVideoToggle.setToggleState (false, juce::dontSendNotification);
    combineVideoToggle.onClick = [this]
    { if (onCombineVideoToggled) onCombineVideoToggled (combineVideoToggle.getToggleState()); };
    addAndMakeVisible (combineVideoToggle);

    deliveryLabel.setText ("Aim the loudness at", juce::dontSendNotification);
    addAndMakeVisible (deliveryLabel);

    deliveryCombo.onChange = [this]
    {
        if (! onDeliveryTargetChanged)
            return;

        // Item 1 is "Not delivering anywhere", which is the absence of a
        // choice rather than a choice -- so it reports as empty.
        const int index = deliveryCombo.getSelectedId() - 2;
        onDeliveryTargetChanged (index >= 0 && index < deliveryNames.size()
                                     ? deliveryNames[index] : juce::String());
    };
    addAndMakeVisible (deliveryCombo);

    deliveryNote.setText ("Every streaming service turns everything it plays to the same "
                          "loudness, so how loud you record decides what people hear -- "
                          "and the peak meters can't tell you. Nothing is changed for you: "
                          "this measures the mix and says which way to move.",
                          juce::dontSendNotification);
    deliveryNote.setJustificationType (juce::Justification::topLeft);
    deliveryNote.setMinimumHorizontalScale (1.0f);
    deliveryNote.setFont (juce::Font (12.0f));
    deliveryNote.setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
    addAndMakeVisible (deliveryNote);

    loudnessAdviceLabel.setJustificationType (juce::Justification::topLeft);
    loudnessAdviceLabel.setMinimumHorizontalScale (1.0f);
    loudnessAdviceLabel.setFont (juce::Font (13.0f));
    loudnessAdviceLabel.setColour (juce::Label::textColourId, AppLookAndFeel::accent);
    addAndMakeVisible (loudnessAdviceLabel);

    combineVideoNote.setJustificationType (juce::Justification::topLeft);
    combineVideoNote.setFont (juce::Font (12.0f));
    addAndMakeVisible (combineVideoNote);

    destinationFolderButton.onClick = [this] { if (onDestinationFolderClicked) onDestinationFolderClicked(); };
    addAndMakeVisible (destinationFolderButton);

    diagnosticsExportButton.onClick = [this] { if (onDiagnosticsExportClicked) onDiagnosticsExportClicked(); };
    addAndMakeVisible (diagnosticsExportButton);

    closeButton.onClick = [this] { if (onCloseClicked) onCloseClicked(); };
    addAndMakeVisible (closeButton);

    // Four headings over what was a flat list. The reader can now find the
    // storage picker by scanning four words instead of reading fifteen rows.
    const std::pair<juce::Label*, const char*> sections[] = {
        { &storageSection, "WHERE RECORDINGS GO" },
        { &formatSection,  "RECORDING FORMAT" },
        { &deliverySection, "WHERE IT'S GOING" },
        { &micSection,     "MICROPHONES" },
        { &outputSection,  "MONITORING AND OUTPUT" },
    };

    for (auto& [label, text] : sections)
    {
        label->setText (text, juce::dontSendNotification);
        label->setFont (juce::Font (10.0f, juce::Font::bold));
        label->setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
        addAndMakeVisible (label);
    }

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
        "Every USB microphone runs on its own crystal, and no two tick at exactly "
        "the same rate. One is chosen as the reference and the others are "
        "continuously nudged to match it -- that keeps the tracks lined up over "
        "a long take. Leave this alone unless one mic is being more dramatic "
        "than the rest.",
        juce::dontSendNotification);
    clockMasterHelpLabel.setJustificationType (juce::Justification::topLeft);
    clockMasterHelpLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (clockMasterHelpLabel);

    // The name other apps see this rig under. Applied when typing finishes,
    // not per keystroke -- each change replaces a device other apps may be
    // recording from.
    addAndMakeVisible (aggregateNameLabel);
    aggregateNameEditor.setTextToShowWhenEmpty ("SobStage", juce::Colours::grey);
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

void AdvancedPanel::setMicSelections (const std::vector<MicChoice>& mics)
{
    // Rebuilt only when the set of names changes. The panel repaints at 2 Hz,
    // and recreating the toggles every tick would fight the user's click.
    juce::StringArray incoming;
    for (const auto& m : mics)
        incoming.add (m.label);

    if (incoming != lastMicNames)
    {
        micToggles.clear();
        lastMicNames = incoming;

        for (const auto& m : mics)
        {
            auto toggle = std::make_unique<juce::ToggleButton> (m.label);

            // The DEVICE name, not the label: the label carries the microphone
            // count, and the app looks the device up by its own name.
            const auto name = m.deviceName;

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
        micToggles[i]->setToggleState (mics[i].enabled, juce::dontSendNotification);
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
    g.fillAll (AppLookAndFeel::surface);

    // One hairline under each heading, in the outline tone. Anything heavier
    // turns a grouping cue into four more lines competing with the settings.
    g.setColour (AppLookAndFeel::outline);
    const auto bounds = getLocalBounds().reduced (12, 0);

    for (int y : ruleYs)
        g.fillRect (bounds.getX(), y, bounds.getWidth(), 1);
}

int AdvancedPanel::getRequiredHeight() const
{
    // Mirrors resized() below. Kept as an explicit sum rather than measured
    // from a trial layout, because resized() consumes the bounds it is given
    // and cannot report what it would have wanted from a taller one.
    constexpr int kMargins       = 12 * 2;
    constexpr int kCloseButton   = 30 + 14;
    constexpr int kSection       = 14 + 3 + 1 + 9;  // heading, gap, rule, gap
    constexpr int kRow           = 26 + 4;
    constexpr int kMicListLabel  = 22;
    constexpr int kMicToggle     = 24 + 2;
    constexpr int kClockHelp     = 64 + 8;
    constexpr int kDrift         = 60 + 4;
    constexpr int kTrimViewport  = 100 + 16;
    constexpr int kAggregate     = 20 + 16;
    constexpr int kMirror        = 26 + 16;
    constexpr int kDiagnostics   = 30;

    // save-to volume, destination folder, sample rate, bit depth, buffer size,
    // latency, delivery target, clock master, output device, backend,
    // aggregate name.
    constexpr int kRowCount = 11;

    // The gaps resized() leaves between sections that are not a heading's own:
    // after the format rows, after the delivery block, and after the microphone
    // checkbox list.
    constexpr int kSectionGaps = 12 + 12 + 10;

    // The backup copy's note, the combined-video toggle and its note.
    constexpr int kMirrorNote  = 20;
    constexpr int kCombine     = 26 + 32;

    // "Where it's going": the explanation and the line of advice under it.
    constexpr int kDelivery    = 56 + 4 + 36;

    return kMargins + kCloseButton + (kSection * 5) + (kRow * kRowCount)
         + kMicListLabel + static_cast<int> (micToggles.size()) * kMicToggle
         + kSectionGaps + kClockHelp + kDrift + kTrimViewport + kAggregate
         + kMirror + kMirrorNote + kCombine + kDelivery + kDiagnostics;
}

void AdvancedPanel::resized()
{
    auto area = getLocalBounds().reduced (12);
    ruleYs.clear();

    // Top-left and first in the layout, where a back control is looked for,
    // and placed before anything else claims the space so it cannot be pushed
    // off the bottom by a long device list.
    closeButton.setBounds (area.removeFromTop (30).removeFromLeft (110));
    area.removeFromTop (14);

    // A heading, then the hairline paint() draws under it. The gap below the
    // rule is wider than the one above the next heading, so a section reads as
    // one block rather than as rows that happen to be adjacent.
    auto section = [&] (juce::Label& heading) {
        heading.setBounds (area.removeFromTop (14));
        area.removeFromTop (3);
        ruleYs.push_back (area.getY());
        area.removeFromTop (9);
    };

    // The value column is capped rather than taking the whole right half. At
    // half the panel a combo holding "root - 29.8 GB free" stretched to 340px
    // of mostly empty well, and short labels sat a long way from their values.
    constexpr int kValueWidth = 300;

    auto row = [&] (juce::Label& label, juce::Component& value) {
        auto r = area.removeFromTop (26);
        value.setBounds (r.removeFromRight (juce::jmin (kValueWidth, r.getWidth() * 3 / 5)));
        r.removeFromRight (12);
        label.setBounds (r);
        area.removeFromTop (4);
    };

    // Where recordings go comes first, above the read-only format rows. It is
    // the choice a user comes in here to make -- picking an SD card before a
    // take -- and at the bottom of the panel it was below the fold, found only
    // by scrolling past four values nobody can change.
    section (storageSection);
    row (storageLabel, storageCombo);
    row (destinationFolderLabel, destinationFolderButton);

    // The backup copy is a storage decision, so it belongs with the other two
    // rather than orphaned at the bottom between the aggregate device and the
    // diagnostics button.
    mirrorToggle.setBounds (area.removeFromTop (26));
    mirrorNote.setBounds (area.removeFromTop (20).reduced (20, 0));

    // With the backup copy, because both are answers to "what else ends up on
    // my disk when I stop".
    combineVideoToggle.setBounds (area.removeFromTop (26));

    // Only takes room when it has something to say, so the panel does not
    // carry an empty line for everyone whose machine is set up correctly.
    combineVideoNote.setBounds (combineVideoNote.getText().isEmpty()
                                    ? juce::Rectangle<int>()
                                    : area.removeFromTop (32).reduced (20, 0));

    area.removeFromTop (16);

    section (formatSection);
    row (sampleRateLabel, sampleRateValue);
    row (bitDepthLabel, bitDepthValue);
    row (bufferSizeLabel, bufferSizeValue);
    row (latencyLabel, latencyValue);
    area.removeFromTop (12);

    section (deliverySection);
    row (deliveryLabel, deliveryCombo);
    deliveryNote.setBounds (area.removeFromTop (56));
    area.removeFromTop (4);
    loudnessAdviceLabel.setBounds (area.removeFromTop (36));
    area.removeFromTop (12);

    section (micSection);
    micSelectionLabel.setBounds (area.removeFromTop (22));
    for (auto& toggle : micToggles)
    {
        toggle->setBounds (area.removeFromTop (24).reduced (8, 0));
        area.removeFromTop (2);
    }
    area.removeFromTop (10);

    row (clockMasterLabel, clockMasterCombo);
    clockMasterHelpLabel.setBounds (area.removeFromTop (64));
    area.removeFromTop (8);

    driftLabel.setBounds (area.removeFromTop (60));
    area.removeFromTop (4);

    trimViewport.setBounds (area.removeFromTop (100));
    layOutTrimRows();
    area.removeFromTop (16);

    section (outputSection);
    row (outputDeviceLabel, outputDeviceCombo);
    row (backendLabel, backendValue);
    row (aggregateNameLabel, aggregateNameEditor);
    aggregateStatusLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (16);

    diagnosticsExportButton.setBounds (area.removeFromTop (30).removeFromLeft (180));
}


void AdvancedPanel::setCombineVideoState (bool on, const juce::String& unavailableReason)
{
    combineVideoToggle.setToggleState (on, juce::dontSendNotification);

    // §10.6: what is missing and what to do about it, under the control it
    // affects. The toggle stays usable either way -- switching it on with no
    // ffmpeg installed is a reasonable thing to do just before installing it,
    // and a disabled control with an explanation nobody can act on is worse
    // than an enabled one that says what it needs.
    combineVideoNote.setText (unavailableReason, juce::dontSendNotification);
    resized();
}


void AdvancedPanel::setDeliveryTargets (const juce::StringArray& names, const juce::String& chosen)
{
    if (names != deliveryNames)
    {
        deliveryNames = names;
        deliveryCombo.clear (juce::dontSendNotification);

        // First, and the default: most takes are not being delivered anywhere,
        // and telling someone recording a rehearsal that they are 8 dB under
        // Spotify is noise.
        deliveryCombo.addItem ("Not delivering anywhere", 1);

        for (int i = 0; i < names.size(); ++i)
            deliveryCombo.addItem (names[i], i + 2);
    }

    const int index = deliveryNames.indexOf (chosen);
    deliveryCombo.setSelectedId (index >= 0 ? index + 2 : 1, juce::dontSendNotification);
}

void AdvancedPanel::setLoudnessAdvice (const juce::String& text)
{
    loudnessAdviceLabel.setText (text, juce::dontSendNotification);
}

} // namespace mma
