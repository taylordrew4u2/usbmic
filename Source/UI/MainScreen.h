#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "SkullMeterComponent.h"
#include "MixBarComponent.h"

namespace mma {

/// §10.2: the main screen. One window, one primary control (record button),
/// everything else is status. No audio jargon in this view (§10.2) -- string
/// literals here are deliberately plain-language.
class MainScreen : public juce::Component
{
public:
    MainScreen();
    ~MainScreen() override;

    void resized() override;
    void paint (juce::Graphics& g) override;

    /// Rebuilds the skull row for the given mic count. Ownership of the
    /// SkullMeterComponents stays with this class.
    void setMicCount (int count);
    SkullMeterComponent* getSkullMeter (int index);

    void setRecording (bool isRecording);
    void setElapsedTimeText (const juce::String& text) { elapsedLabel.setText (text, juce::dontSendNotification); }
    void setRemainingTimeText (const juce::String& text) { remainingLabel.setText (text, juce::dontSendNotification); }
    void setSaveLocationText (const juce::String& text) { saveLocationLabel.setText (text, juce::dontSendNotification); }
    void setNoMicsMessage (bool show);
    void setRecordButtonEnabled (bool enabled, const juce::String& disabledReason);

    std::function<void()> onRecordButtonClicked;
    std::function<void (double)> onVolumeChanged; // 0-100
    std::function<void()> onAdvancedClicked;
    std::function<void()> onMuteToggled;

private:
    juce::OwnedArray<SkullMeterComponent> skullMeters;
    MixBarComponent mixBar;

    juce::TextButton recordButton { "Start recording" };
    juce::Label elapsedLabel, remainingLabel, saveLocationLabel, noMicsLabel, disabledReasonLabel;
    juce::Slider volumeSlider;
    juce::ToggleButton muteButton { "Mute" };
    juce::TextButton advancedButton { "Advanced" };

    bool recording = false;

    static const juce::Colour kBackground;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainScreen)
};

} // namespace mma
