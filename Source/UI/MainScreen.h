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

    /// How many channel strips currently exist. Callers rebinding meter
    /// pointers need this to size the set before they bind, never after.
    int getMicCount() const;
    SkullMeterComponent* getSkullMeter (int index);

    void setMixMetering (Metering* meteringSource) { mixBar.setMetering (meteringSource); }
    /// Repaints only the meters, so status-label updates don't redraw the whole screen.
    void repaintMeters();

    void setRecording (bool isRecording);
    void setElapsedTimeText (const juce::String& text) { elapsedLabel.setText (text, juce::dontSendNotification); }
    void setRemainingTimeText (const juce::String& text) { remainingLabel.setText (text, juce::dontSendNotification); }
    void setSaveLocationText (const juce::String& text) { saveLocationLabel.setText (text, juce::dontSendNotification); }
    void setNoMicsMessage (bool show);
    void setRecordButtonEnabled (bool enabled, const juce::String& disabledReason);

    /// §14.6: light the skull of the mic currently being heard (-1 for none).
    void setHighlightedMic (int index);

    /// The mute button always tells the truth about the bus, including the
    /// §5 runaway cut, which the user must be able to undo from here.
    void setMuteState (bool muted, bool runawayMuted);

    /// §6.2: the user's name for the next take. Empty is fine.
    juce::String getSessionName() const { return sessionNameEditor.getText(); }

    /// §10.5/§6.5/§6.6: the single most serious thing worth telling the user
    /// about the rig right now. Empty hides the line.
    void setAdviceText (const juce::String& text);

    /// §5.3/§5.4: anything wrong with the listening path, in plain language.
    /// Empty hides the line. Never leave this unshown -- the spec forbids
    /// silently delivering a high-latency mix instead of saying so.
    void setMonitorProblemText (const juce::String& text);

    std::function<void()> onRecordButtonClicked;
    std::function<void (double)> onVolumeChanged; // 0-100
    std::function<void()> onAdvancedClicked;
    std::function<void()> onMuteToggled;
    std::function<void (int)> onMicNameClicked; // skull index

private:
    juce::OwnedArray<SkullMeterComponent> skullMeters;
    MixBarComponent mixBar;

    juce::TextButton recordButton { "Start recording" };
    juce::Label elapsedLabel, remainingLabel, saveLocationLabel, noMicsLabel, disabledReasonLabel;
    juce::Label monitorProblemLabel, adviceLabel;
    juce::Slider volumeSlider;
    juce::TextEditor sessionNameEditor;
    juce::ToggleButton muteButton { "Mute" };
    juce::TextButton advancedButton { "Advanced" };

    bool recording = false;

    static const juce::Colour kBackground;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainScreen)
};

} // namespace mma
