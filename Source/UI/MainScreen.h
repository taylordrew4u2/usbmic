#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
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

    /// The height this screen would want if the window were not limiting it --
    /// the camera picture at the size the user chose, rather than the size it
    /// has been squeezed to. What an owner deciding how big to open or grow the
    /// window needs, since getRequiredHeight() is already clamped to the window
    /// and so can never ask for more room than it has.
    int getPreferredHeight() const;

    /// How much of this screen the user can actually SEE -- the viewport's
    /// height, not this component's own.
    ///
    /// The two are not the same, and the difference was a feedback loop. The
    /// camera picture is given the height left over after everything else, and
    /// it took that from getHeight(). But the owner sizes this component to
    /// max(viewport, getRequiredHeight()) -- so a taller picture grew the
    /// canvas, which offered more height, which grew the picture. At the size
    /// the window actually opens at that settled with 1007px of content inside
    /// a 560px window: the picture overflowed and the record button sat below
    /// the fold, which is the one control nobody can afford to go looking for.
    ///
    /// Sizing the picture against the viewport breaks the loop, because this is
    /// an input from the owner rather than something derived from the content.
    /// Unset (0) means "no owner has said", and getHeight() stands.
    void setVisibleHeight (int height);
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

    /// One camera that is switched on for this take, as the main screen needs
    /// it: something to look at, and whose it is.
    struct CameraTile
    {
        std::string id;
        juce::String displayName;
    };

    /// §10.2: the picture belongs beside the levels, not behind a door.
    ///
    /// Framing and gain are one job -- you set them together, and during a take
    /// you watch them together. Sending someone to another screen to see the
    /// camera meant they could see the shot or the meters but never both, and
    /// the one moment that matters is the one where a mic has gone quiet while
    /// the shot still looks fine.
    ///
    /// Only the cameras actually switched on appear. A rig with none is laid out
    /// exactly as before, so this costs an audio-only user no space at all.
    void setCameraTiles (const std::vector<CameraTile>& tiles);

    /// Makes the live view for one camera. Supplied by the owner, which is the
    /// only thing holding the open devices; nullptr is normal and means that
    /// camera is not open.
    std::function<std::unique_ptr<juce::Component> (const std::string&)> makeViewer;

    /// §10.2: how big the pictures are drawn, as a step into a fixed size
    /// table. One camera across a table wants a bigger picture than four in a
    /// row, and which of those the user is doing is not something the app can
    /// work out for them -- so it is a control rather than a guess.
    ///
    /// Clamped to the table, so a remembered value from a future version cannot
    /// put the row somewhere the layout cannot draw.
    void setCameraScale (int step);
    int getCameraScale() const noexcept { return cameraScale; }
    static int getCameraScaleStepCount() noexcept;

    /// The arrows. Fired by the buttons on the row and by the up/down keys.
    std::function<void (int)> onCameraScaleChanged;

    /// Destroys the live views without disturbing anything else.
    ///
    /// The rule this and setCameraTiles() together keep: whichever screen is on
    /// screen owns the viewers, and no camera ever has two. The panels are
    /// mutually exclusive viewports, so the owner releases these before showing
    /// another screen and repopulates them on the way back. A viewer outliving
    /// the device behind it is a component drawing from freed memory.
    void releaseCameraViews();

    /// §5.3/§5.4: anything wrong with the listening path, in plain language.
    /// Empty hides the line. Never leave this unshown -- the spec forbids
    /// silently delivering a high-latency mix instead of saying so.
    void setMonitorProblemText (const juce::String& text);

    /// The band the problem line was given, so a headless probe can prove the
    /// message is not being clipped.
    int getMonitorProblemBandHeight() const noexcept { return monitorProblemHeight(); }

    /// The running version, shown beside the tagline. On screen rather than
    /// behind a menu because "which build am I running" is the first question
    /// asked when a change appears not to have arrived, and an answer that
    /// takes hunting is an answer nobody checks.
    void setVersionText (const juce::String& text);

    /// §10.2: the camera's own controls, brought onto the main screen.
    /// "Full preview" is the existing preview-quality choice; "+ Add camera"
    /// opens the panel where cameras are switched on and named.
    std::function<void (bool)> onFullPreviewToggled;
    void setFullPreview (bool on) { fullPreviewToggle.setToggleState (on, juce::dontSendNotification); }

    std::function<void()> onRecordButtonClicked;
    std::function<void (double)> onVolumeChanged; // 0-100
    std::function<void()> onAdvancedClicked;
    std::function<void()> onCamerasClicked;
    std::function<void()> onHelpClicked;
    std::function<void()> onMuteToggled;
    std::function<void (int)> onMicNameClicked; // skull index

private:
    // The masthead. A window with no name in it is a window you have to
    // remember the name of, and the tagline is the one place the app gets to
    // say what it is for before anyone presses anything.
    juce::Label brandLabel, taglineLabel, versionLabel;

    // Section headings, so the screen reads as two things -- the picture and
    // the sound -- rather than one undifferentiated stack of controls.
    juce::Label cameraSectionLabel, micSectionLabel;

    juce::ToggleButton fullPreviewToggle { "Full preview" };
    juce::TextButton addCameraButton { "+ Add camera" };

    /// The sob mark, drawn rather than loaded: it is four shapes, and a PNG
    /// would need a second copy of the icon kept in step with Tools/make_icon.py
    /// by hand.
    void paintBrandMark (juce::Graphics& g, juce::Rectangle<float> bounds) const;

    /// Where the mark goes, set by resized() and read by paint().
    juce::Rectangle<int> brandMarkBounds;

    /// The REC badge over a live picture, and the rectangles to draw it in --
    /// one per camera tile, filled during layout.
    std::vector<juce::Rectangle<int>> cameraRecBadges;

    juce::OwnedArray<SkullMeterComponent> skullMeters;
    MixBarComponent mixBar;

    struct CameraView
    {
        std::string id;
        // Owned here and destroyed with the tile, per releaseCameraViews().
        std::unique_ptr<juce::Component> viewer;
        std::unique_ptr<juce::Label> caption;
        std::unique_ptr<juce::Label> placeholder;
    };

    std::vector<CameraView> cameraViews;

    int cameraScale = 1;

    /// The viewport height the owner last reported; 0 until it does.
    int visibleHeight = 0;

    /// The height the camera picture is measured against: what the user can
    /// see, falling back to this component's own height before any owner has
    /// said otherwise.
    int layoutHeight() const noexcept;
    juce::TextButton cameraSmallerButton { juce::String::charToString (juce::juce_wchar (0x25bc)) };
    juce::TextButton cameraLargerButton { juce::String::charToString (juce::juce_wchar (0x25b2)) };
    juce::Label cameraSizeLabel;

    /// Tile width at the current step, and the rows the row needs to lay them
    /// out in. Kept together because the height depends on both: enlarging past
    /// what fits across wraps rather than shrinking back, which is what makes
    /// the arrows do something on a four-camera rig.
    /// The tile width for a step, given the width the screen has to spend and
    /// the height it has left after everything else is laid out.
    ///
    /// Both bounds matter. Width alone lets a picture grow until the record
    /// button is pushed off the bottom of the window, which is the failure this
    /// screen exists to avoid; height alone ignores what the user asked for by
    /// making the window wider.
    int cameraTileWidthFor (int step, int availableWidth, int availableHeight) const noexcept;

    /// Everything resized() lays out that is not the camera row. The camera
    /// gets what is left, which is what makes a taller window a bigger picture.
    int nonCameraHeight() const noexcept;
    int cameraRowsNeeded (int tileWidth, int availableWidth) const noexcept;

    // Carries the current tile row across iterations of the layout loop. A
    // member rather than a local because the loop consumes it tile by tile and
    // starts a fresh one every perRow.
    juce::Rectangle<int> cameraRowScratch;
    // What the tiles were last built from, so the UI tick can call
    // setCameraTiles() every frame without tearing down live views that have
    // not changed. Rebuilding a viewer per frame would flicker and churn the
    // device.
    std::vector<std::string> lastTileIds;

    /// The band the monitor-problem line needs, grown to fit its message.
    ///
    /// The message names a cause and what to do about it, which does not fit on
    /// one line at this width. Clipping it leaves exactly the dead end the
    /// reason exists to end, so the band is measured from the text.
    int monitorProblemHeight() const noexcept;

    /// Height the camera row needs, or zero when no camera is switched on.
    int cameraRowHeight() const;

    /// The camera row's height given a specific amount of leftover space, where
    /// 0 means "unconstrained". Shared by the clamped and preferred paths so
    /// they cannot drift apart.
    int cameraRowHeightForSpare (int spare) const;

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
    // The third door. Beside Settings in the masthead, because someone with a
    // flat meter looks up there for a way out, not down in the level row.
    juce::TextButton helpButton { "Help" };

    /// Layout differs between the two states, so it is remembered rather than
    /// re-derived from a label's text. setRecording() is called from the UI
    /// tick, so it only re-lays the screen out when this actually flips.
    bool recording = false;

    static const juce::Colour kBackground;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainScreen)
};

} // namespace mma
