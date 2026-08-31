#include "MainScreen.h"
#include "AppLookAndFeel.h"

namespace mma {

const juce::Colour MainScreen::kBackground { palette::background };

MainScreen::MainScreen()
{
    addAndMakeVisible (mixBar);

    // The one thing on this screen someone came here to press. Flat and
    // in-palette like everything else, but filled with the accent so it is
    // unmistakably the primary action -- minimal is not the same as invisible,
    // and before this it read as another dark slab.
    recordButton.setColour (juce::TextButton::buttonColourId, AppLookAndFeel::accent);
    recordButton.setColour (juce::TextButton::textColourOffId, AppLookAndFeel::background);
    recordButton.onClick = [this] { if (onRecordButtonClicked) onRecordButtonClicked(); };
    addAndMakeVisible (recordButton);

    elapsedLabel.setJustificationType (juce::Justification::centred);
    remainingLabel.setJustificationType (juce::Justification::centred);
    saveLocationLabel.setJustificationType (juce::Justification::centred);

    // A quiet supporting tier under the record button, rather than three lines
    // competing with it and with each other at the same weight.
    for (auto* l : { &elapsedLabel, &remainingLabel, &saveLocationLabel, &filesSavingLabel })
    {
        l->setFont (juce::Font (12.0f));
        l->setColour (juce::Label::textColourId, AppLookAndFeel::secondary);
    }
    addAndMakeVisible (elapsedLabel);
    addAndMakeVisible (remainingLabel);
    addAndMakeVisible (saveLocationLabel);

    // Green rather than the supporting grey the line above it uses: while a
    // take is running this is the one status line saying the thing the user
    // most wants to be true, which is that files are appearing.
    filesSavingLabel.setColour (juce::Label::textColourId, AppLookAndFeel::meterLow);
    addAndMakeVisible (filesSavingLabel);

    noMicsLabel.setText ("Plug in a microphone to get started.", juce::dontSendNotification);
    noMicsLabel.setJustificationType (juce::Justification::centred);
    addChildComponent (noMicsLabel);

    adviceLabel.setJustificationType (juce::Justification::centred);
    adviceLabel.setColour (juce::Label::textColourId, AppLookAndFeel::secondary);
    adviceLabel.setFont (juce::Font (12.0f));
    addChildComponent (adviceLabel);

    monitorProblemLabel.setJustificationType (juce::Justification::centred);
    // Was JUCE's stock orange at the default size, running the full width of the
    // window in one thin line. Larger now, and allowed to wrap so it reads as a
    // sentence rather than a ticker.
    //
    // Warning amber rather than the accent: the accent fills the record button,
    // so painting a problem in it said "press me" in the colour of the thing
    // that had just gone wrong.
    monitorProblemLabel.setColour (juce::Label::textColourId, AppLookAndFeel::warning);
    monitorProblemLabel.setFont (juce::Font (13.0f));
    addChildComponent (monitorProblemLabel);

    // Why the record button will not go. Warning amber and small, matching the
    // monitor-problem line it sits next to, rather than primary-text white,
    // which read as an ordinary status line.
    disabledReasonLabel.setJustificationType (juce::Justification::centred);
    disabledReasonLabel.setColour (juce::Label::textColourId, AppLookAndFeel::warning);
    disabledReasonLabel.setFont (juce::Font (12.0f));
    addChildComponent (disabledReasonLabel);

    // The bare number told a user nothing about what it controlled. Naming it
    // inside the value box costs no layout and needs no separate label; a
    // suffix would read "70 monitor", so the name goes in front.
    volumeSlider.textFromValueFunction = [] (double value)
    {
        return "Monitor " + juce::String (juce::roundToInt (value));
    };
    volumeSlider.valueFromTextFunction = [] (const juce::String& text)
    {
        return text.retainCharacters ("0123456789").getDoubleValue();
    };
    volumeSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, true, 86, 20);
    volumeSlider.setRange (0.0, 100.0, 1.0);
    volumeSlider.setValue (70.0); // §5.1 default
    volumeSlider.onValueChange = [this] { if (onVolumeChanged) onVolumeChanged (volumeSlider.getValue()); };
    addAndMakeVisible (volumeSlider);

    muteButton.setClickingTogglesState (false); // state comes from the bus, not the click
    muteButton.onClick = [this] { if (onMuteToggled) onMuteToggled(); };
    addAndMakeVisible (muteButton);

    // §6.2: naming the take is optional and never a gate -- the placeholder
    // says what happens if it is left alone.
    sessionNameEditor.setTextToShowWhenEmpty ("Session name (optional)", juce::Colours::grey);
    sessionNameEditor.setJustification (juce::Justification::centredLeft);
    addAndMakeVisible (sessionNameEditor);

    advancedButton.onClick = [this] { if (onAdvancedClicked) onAdvancedClicked(); };
    addAndMakeVisible (advancedButton);

    camerasButton.onClick = [this] { if (onCamerasClicked) onCamerasClicked(); };
    addAndMakeVisible (camerasButton);
}

MainScreen::~MainScreen() = default;

void MainScreen::repaintMeters()
{
    for (auto* meter : skullMeters)
        meter->repaint();

    mixBar.repaint();
}

void MainScreen::setMicCount (int count)
{
    skullMeters.clear();
    for (int i = 0; i < count; ++i)
    {
        auto* meter = new SkullMeterComponent();
        meter->onNameClicked = [this, i] { if (onMicNameClicked) onMicNameClicked (i); };
        addAndMakeVisible (meter);
        skullMeters.add (meter);
    }
    setNoMicsMessage (count == 0);
    resized();
}

int MainScreen::getRequiredHeight() const
{
    // The channel-strip row plus every fixed row resized() lays out beneath it,
    // and the margins around the lot.
    return 522 + 24;
}

int MainScreen::getMicCount() const
{
    return skullMeters.size();
}

SkullMeterComponent* MainScreen::getSkullMeter (int index)
{
    return skullMeters[index];
}

void MainScreen::setRecording (bool isRecording)
{
    const bool changed = recording != isRecording;
    recording = isRecording;
    // §10.4/§10.6: buttons say what happens. "Start recording" -> "Recording."
    recordButton.setButtonText (recording ? "Recording. Tap to stop." : "Start recording");

    // Cyan to start, red while running: the colour carries the state at a
    // glance, and §9.3 keeps the text saying it too rather than colour alone.
    recordButton.setColour (juce::TextButton::buttonColourId,
                            recording ? AppLookAndFeel::danger : AppLookAndFeel::accent);
    recordButton.setColour (juce::TextButton::textColourOffId,
                            recording ? AppLookAndFeel::bone : AppLookAndFeel::background);

    // The status row is split in two while recording and single when not, so
    // the change of state is a change of layout. Without this the elapsed-time
    // label kept the empty bounds resized() gave it in the idle layout, and
    // "Recording for 4m 12s" was set on a label nobody could see.
    if (changed)
        resized();
}

void MainScreen::setHighlightedMic (int index)
{
    for (int i = 0; i < skullMeters.size(); ++i)
        skullMeters[i]->setHighlighted (i == index);
}

void MainScreen::setMuteState (bool muted, bool runawayMuted)
{
    muteButton.setToggleState (muted, juce::dontSendNotification);

    // §10.6: the button says what pressing it will do. After a runaway cut it
    // is the recovery control, and it must say so.
    muteButton.setButtonText (runawayMuted ? "Unmute (sound was cut)"
                                           : (muted ? "Unmute" : "Mute"));
}

void MainScreen::setCameraCount (int count)
{
    // §9.3: the count is on the button rather than only in a colour, so a
    // camera being in the take is readable at a glance from the main screen.
    camerasButton.setButtonText (count > 0 ? "Cameras (" + juce::String (count) + ")" : "Cameras");
}

void MainScreen::setAdviceText (const juce::String& text)
{
    adviceLabel.setText (text, juce::dontSendNotification);
    adviceLabel.setVisible (text.isNotEmpty());
}

void MainScreen::setMonitorProblemText (const juce::String& text)
{
    monitorProblemLabel.setText (text, juce::dontSendNotification);
    monitorProblemLabel.setVisible (text.isNotEmpty());
}

void MainScreen::setNoMicsMessage (bool show)
{
    noMicsLabel.setVisible (show);
}

void MainScreen::setRecordButtonEnabled (bool enabled, const juce::String& disabledReason)
{
    recordButton.setEnabled (enabled);
    disabledReasonLabel.setText (disabledReason, juce::dontSendNotification);

    // resized() only reserves the reason row when it is visible, so the layout
    // has to be redone when that changes. Guarded because this is called from
    // a timer, and re-laying out the screen every tick is not free.
    if (disabledReasonLabel.isVisible() == enabled)
    {
        disabledReasonLabel.setVisible (! enabled);
        resized();
    }
}

void MainScreen::paint (juce::Graphics& g)
{
    g.fillAll (kBackground);
}

void MainScreen::resized()
{
    auto area = getLocalBounds().reduced (16);

    // Channel strips: tall and narrow, sitting side by side like a mixing
    // desk, rather than short wide boxes stretched to fill the width.
    auto skullRow = area.removeFromTop (230);
    if (skullMeters.isEmpty())
    {
        noMicsLabel.setBounds (skullRow);
    }
    else
    {
        // A strip has a natural width and keeps it. Dividing the full width by
        // the channel count made two microphones render as two enormous panels
        // and eight as slivers; a desk's channels are the same size whether
        // four are in use or twenty.
        // A strip has a preferred width and a ceiling, rather than one fixed
        // size. At 84px flat, three microphones sat in a narrow cluster with
        // most of the window empty on either side; letting them grow towards
        // 132px fills that space without ever stretching two mics into two
        // billboards the way dividing the full width used to.
        constexpr int kPreferredStripWidth = 96;
        constexpr int kMaxStripWidth       = 132;
        constexpr int kMinStripWidth       = 52;

        const int count = juce::jmax (1, skullMeters.size());
        const int available = skullRow.getWidth();

        int meterWidth = juce::jlimit (kMinStripWidth, kMaxStripWidth, available / count);
        meterWidth = juce::jmax (meterWidth, juce::jmin (kPreferredStripWidth, available / count));

        const int wanted = meterWidth * count;

        // Centred as a block, so a rig of two sits in the middle rather than
        // hugging the left edge.
        if (wanted < available)
            skullRow.removeFromLeft ((available - wanted) / 2);
        for (auto* meter : skullMeters)
            meter->setBounds (skullRow.removeFromLeft (meterWidth).reduced (2, 0));
    }

    area.removeFromTop (10);
    mixBar.setBounds (area.removeFromTop (28));
    area.removeFromTop (14);

    sessionNameEditor.setBounds (area.removeFromTop (28).reduced (area.getWidth() / 6, 0));
    area.removeFromTop (8);
    recordButton.setBounds (area.removeFromTop (56));

    // Only take the row when there is something in it. Reserved unconditionally
    // it left a 28px hole under the record button whenever recording was
    // possible -- which is almost always -- and pushed the status lines away
    // from the button they belong to.
    if (disabledReasonLabel.isVisible())
    {
        disabledReasonLabel.setBounds (area.removeFromTop (20));
        area.removeFromTop (8);
    }
    else
    {
        disabledReasonLabel.setBounds ({});
        area.removeFromTop (14);
    }

    // Two centred halves while recording, one centred line when not. Splitting
    // unconditionally left the remaining-time text centred in the right-hand
    // half -- so it sat off-centre under a centred button, next to a centred
    // "Saves to" line, for no reason a reader could see.
    auto statusRow = area.removeFromTop (24);

    if (recording)
    {
        elapsedLabel.setBounds (statusRow.removeFromLeft (statusRow.getWidth() / 2));
        remainingLabel.setBounds (statusRow);
    }
    else
    {
        elapsedLabel.setBounds ({});
        remainingLabel.setBounds (statusRow);
    }

    saveLocationLabel.setBounds (area.removeFromTop (20));
    filesSavingLabel.setBounds (area.removeFromTop (18));
    monitorProblemLabel.setBounds (area.removeFromTop (20));
    adviceLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (8);

    auto bottomRow = area.removeFromTop (32);

    // The slider gives up a third of its old width to make room for the second
    // door. It was two thirds of the row for a control with a 0-100 range;
    // half is still more resolution than the ear has.
    volumeSlider.setBounds (bottomRow.removeFromLeft (bottomRow.getWidth() / 2));
    muteButton.setBounds (bottomRow.removeFromLeft (bottomRow.getWidth() / 3));
    camerasButton.setBounds (bottomRow.removeFromLeft (bottomRow.getWidth() / 2).reduced (2, 0));
    advancedButton.setBounds (bottomRow.reduced (2, 0));
}

} // namespace mma
