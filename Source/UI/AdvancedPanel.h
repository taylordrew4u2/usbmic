#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>
#include <memory>
#include <utility>

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

    /// The height this panel's content needs at its current item count.
    /// The panel is taller than most windows once a few microphones are
    /// listed, so a container scrolls it rather than clipping it -- and a
    /// container can only do that if it is told how tall to make it.
    int getRequiredHeight() const;
    void paint (juce::Graphics& g) override;

    void setSampleRate (double rate) { sampleRateValue.setText (juce::String (rate, 0) + " Hz", juce::dontSendNotification); }
    void setBitDepth (int bits) { bitDepthValue.setText (juce::String (bits) + "-bit", juce::dontSendNotification); }
    void setBufferSize (int samples) { bufferSizeValue.setText (juce::String (samples) + " samples", juce::dontSendNotification); }
    void setMeasuredLatency (double ms) { latencyValue.setText (juce::String (ms, 1) + " ms", juce::dontSendNotification); }
    void setActiveBackendDescription (const juce::String& text) { backendValue.setText (text, juce::dontSendNotification); }
    void setDriftReport (const juce::String& text) { driftLabel.setText (text, juce::dontSendNotification); }

    /// What other apps currently see, and the field that names it. The editor
    /// is only overwritten while unfocused, so refresh never eats typing.
    void setAggregateStatus (const juce::String& text) { aggregateStatusLabel.setText (text, juce::dontSendNotification); }
    void setAggregateName (const juce::String& name)
    {
        if (! aggregateNameEditor.hasKeyboardFocus (true))
            aggregateNameEditor.setText (name, juce::dontSendNotification);
    }
    void setDestinationFolderText (const juce::String& text) { destinationFolderLabel.setText (text, juce::dontSendNotification); }

    /// Fills a combo without firing onChange -- otherwise refreshing the list
    /// would read back as the user having picked something.
    void setOutputDevices (const juce::StringArray& names, const juce::String& selected);
    /// The microphones the OS reports and whether each is currently selected.
    void setMicSelections (const std::vector<std::pair<juce::String, bool>>& mics);

    /// Volumes to offer as save destinations: display name, path, and whether
    /// it is the one currently in use.
    struct VolumeChoice { juce::String label, path; bool current = false; };
    void setStorageVolumes (const std::vector<VolumeChoice>& volumes);

    void setClockMasters (const juce::StringArray& names, const juce::String& selected);

    /// §4: one trim slider per microphone, rebuilt when the mic set changes.
    /// currentTrimDb supplies each row's starting value.
    void setTrimChannels (const juce::StringArray& micNames,
                          const std::function<float (int)>& currentTrimDb);

    std::function<void (int, float)> onTrimChanged; // channel index, dB
    std::function<void (const juce::String&)> onAggregateNameChanged;
    std::function<void()> onDiagnosticsExportClicked;

    /// §10.3 says one door. A door has to open both ways: showing this panel
    /// hides the main screen, and the button that opened it lives there, so
    /// without this the panel is a dead end with no way back.
    std::function<void()> onCloseClicked;
    std::function<void (bool)> onMirrorToggled;

    /// Whether a finished take also writes one video-with-sound file per
    /// camera. Beside the separate files, never instead of them.
    std::function<void (bool)> onCombineVideoToggled;

    /// Sets that toggle and the sentence under it. `unavailableReason` is empty
    /// when the machine can actually do this, and otherwise says what is
    /// missing -- named here, before a take, rather than in an alert after one.
    void setCombineVideoState (bool on, const juce::String& unavailableReason);
    std::function<void()> onDestinationFolderClicked;
    std::function<void (const juce::String&)> onClockMasterChanged;
    std::function<void (const juce::String&, bool)> onMicEnabledChanged;
    std::function<void (const juce::String&)> onStorageVolumeChosen;
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
    std::vector<std::unique_ptr<juce::Label>> trimNameLabels;
    std::vector<std::unique_ptr<juce::Slider>> trimSliders;
    void layOutTrimRows();
    juce::Label outputDeviceLabel;
    juce::ComboBox outputDeviceCombo;
    juce::Label backendLabel, backendValue;
    juce::Label aggregateNameLabel;
    juce::TextEditor aggregateNameEditor;
    juce::Label aggregateStatusLabel;
    juce::ToggleButton mirrorToggle { "Keep a local backup copy" };

    // The reason, under the switch, in the app's voice. §6.3's backup exists
    // for the case where the card does not survive the day, and saying that
    // plainly is what makes someone leave it on.
    juce::Label mirrorNote;
    juce::ToggleButton combineVideoToggle { "Also save video with the sound in one file" };

    // Named under the toggle rather than in an alert, because the answer is
    // "install a thing", and an alert after the take is the wrong moment to
    // learn that.
    juce::Label combineVideoNote;
    juce::Label destinationFolderLabel;
    juce::TextButton destinationFolderButton { "Change..." };
    juce::TextButton diagnosticsExportButton { "Export diagnostics" };
    juce::TextButton closeButton { "< Done" };

    juce::Label micSelectionLabel;
    juce::Label storageLabel;
    juce::ComboBox storageCombo;
    juce::StringArray storagePaths;
    juce::StringArray lastStorageLabels;
    juce::Label clockMasterHelpLabel;

    // Section headings and the hairline rules under them. The panel was one
    // flat list of fifteen rows, so finding the storage picker meant reading
    // every unrelated row on the way past it. resized() records where the
    // rules go; paint() draws them, since a rule is a line rather than a
    // component.
    juce::Label storageSection, formatSection, micSection, outputSection;
    std::vector<int> ruleYs;
    std::vector<std::unique_ptr<juce::ToggleButton>> micToggles;
    juce::StringArray lastMicNames;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdvancedPanel)
};

} // namespace mma
