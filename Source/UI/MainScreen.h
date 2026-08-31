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

    /// The height this screen's content needs. See AdvancedPanel for why a
    /// container asks rather than measures.
    int getRequiredHeight() const;
    SkullMeterComponent* getSkullMeter (int index);

    void setMixMetering (Metering* meteringSource) { mixBar.setMetering (meteringSource); }
    /// Repaints only the meters, so status-label updates don't redraw the whole screen.
    void repaintMeters();

    void setRecording (bool isRecording);
    void setElapsedTimeText (const juce::String& text) { elapsedLabel.setText (text, juce::dontSendNotification); }
    void setRemainingTimeText (const juce::String& text) { remainingLabel.setText (text, juce::dontSendNotification); }
    void setSaveLocationText (const juce::String& text) { saveLocationLabel.setText (text, juce::dontSendNotification); }
    /// §6.2/§10.6: the files this take is putting on disk, growing as they are
    /// written. Empty outside a take. Someone watching this line has already
    /// been answered before they think to ask where the recording went.
    void setFilesBeingSavedText (const juce::String& text) { filesSavingLabel.setText (text, juce::dontSendNotification); }
    void setNoMicsMessage (bool show);
    void setRecordButtonEnabled (bool enabled, const juce::String& disabledReason);

    /// §14.6: light the skull of the mic currently being heard (-1 for none).
    void setHighlightedMic (int index);

    /// The mute button always tells the truth about the bus, including the
    /// §5 runaway cut, which the user must be able to undo from here.
    void setMuteState (bool muted, bool runawayMuted);

    /// §6.2: the user's name for the next take. Empty is fine.
    juce::String getSessionName() const { return sessionNameEditor.getText(); }
    /// Set when the name was given somewhere else -- the pre-record prompt --
    /// so the box on the main screen agrees with the folder that was named.
    void setSessionName (const juce::String& name) { sessionNameEditor.setText (name, juce::dontSendNotification); }

    /// §10.5/§6.5/§6.6: the single most serious thing worth telling the user
    /// about the rig right now. Empty hides the line.
    void setAdviceText (const juce::String& text);

    /// How many cameras are in the take, so the button says whether any are.
    /// Zero leaves it reading "Cameras".
    void setCameraCount (int count);

    /// §5.3/§5.4: anything wrong with the listening path, in plain language.
    /// Empty hides the line. Never leave this unshown -- the spec forbids
    /// silently delivering a high-latency mix instead of saying so.
    void setMonitorProblemText (const juce::String& text);

    std::function<void()> onRecordButtonClicked;
    std::function<void (double)> onVolumeChanged; // 0-100
    std::function<void()> onAdvancedClicked;
    std::function<void()> onCamerasClicked;
    std::function<void()> onMuteToggled;
    std::function<void (int)> onMicNameClicked; // skull index

private:
    juce::OwnedArray<SkullMeterComponent> skullMeters;
    MixBarComponent mixBar;

    juce::TextButton recordButton { "Start recording" };
    juce::Label elapsedLabel, remainingLabel, saveLocationLabel, filesSavingLabel, noMicsLabel, disabledReasonLabel;
    juce::Label monitorProblemLabel, adviceLabel;
    juce::Slider volumeSlider;
    juce::TextEditor sessionNameEditor;
    juce::ToggleButton muteButton { "Mute" };
    // "Settings" rather than "Advanced": this is the only door out of the main
    // screen, and it holds ordinary choices -- which mics are in use, where
    // recordings go -- not expert ones. "Advanced" told users to stay out.
    juce::TextButton advancedButton { "Settings" };
    // The second door, and the only other one. Named for what is behind it,
    // like Settings: a user looking for their webcam looks for "Cameras".
    juce::TextButton camerasButton { "Cameras" };

    /// Layout differs between the two states, so it is remembered rather than
    /// re-derived from a label's text. setRecording() is called from the UI
    /// tick, so it only re-lays the screen out when this actually flips.
    bool recording = false;

    static const juce::Colour kBackground;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainScreen)
};

} // namespace mma
