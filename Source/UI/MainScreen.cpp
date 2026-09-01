#include "MainScreen.h"
#include "AppLookAndFeel.h"
#include <array>

namespace mma {

namespace {
    // 16:9 wells. The default (index 1) is sized so four of them still leave the
    // meters and the record button on screen together -- which is the entire
    // point of putting them here -- and the steps either side exist because that
    // trade-off is not the same for everyone: one camera across a table wants a
    // picture you can judge focus on, four in a row want to fit.
    constexpr int kCameraTileWidths[] = { 120, 176, 248, 340, 456 };
    constexpr int kCameraScaleSteps = static_cast<int> (std::size (kCameraTileWidths));

    constexpr int kCameraCaptionHeight = 16;
    constexpr int kCameraRowGap       = 12;
    constexpr int kCameraTileGap      = 8;

    // The arrows and their label, on their own line above the pictures.
    constexpr int kCameraControlHeight = 22;

    int tileHeightFor (int tileWidth) noexcept
    {
        // 16:9, which is what every webcam this is likely to meet delivers.
        return (tileWidth * 9) / 16;
    }
}

const juce::Colour MainScreen::kBackground { palette::background };

MainScreen::MainScreen()
{
    // The arrows. Small, beside the pictures they resize, and hidden entirely
    // when there are no pictures -- a size control for nothing is clutter.
    cameraSizeLabel.setText ("Camera size", juce::dontSendNotification);
    cameraSizeLabel.setFont (juce::Font (12.0f));
    cameraSizeLabel.setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
    cameraSizeLabel.setJustificationType (juce::Justification::centredRight);
    addChildComponent (cameraSizeLabel);

    cameraSmallerButton.onClick = [this] {
        setCameraScale (cameraScale - 1);
        if (onCameraScaleChanged) onCameraScaleChanged (cameraScale);
    };
    cameraLargerButton.onClick = [this] {
        setCameraScale (cameraScale + 1);
        if (onCameraScaleChanged) onCameraScaleChanged (cameraScale);
    };

    for (auto* b : { &cameraSmallerButton, &cameraLargerButton })
    {
        b->setColour (juce::TextButton::buttonColourId, AppLookAndFeel::surfaceHigh);
        addChildComponent (*b);
    }

    cameraSmallerButton.setEnabled (cameraScale > 0);
    cameraLargerButton.setEnabled (cameraScale < kCameraScaleSteps - 1);

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

int MainScreen::getCameraScaleStepCount() noexcept
{
    return kCameraScaleSteps;
}

int MainScreen::cameraTileWidthAt (int step) const noexcept
{
    return kCameraTileWidths[juce::jlimit (0, kCameraScaleSteps - 1, step)];
}

int MainScreen::cameraRowsNeeded (int tileWidth, int availableWidth) const noexcept
{
    if (cameraViews.empty())
        return 0;

    const int perRow = juce::jmax (1, (availableWidth + kCameraTileGap)
                                          / juce::jmax (1, tileWidth + kCameraTileGap));

    return (static_cast<int> (cameraViews.size()) + perRow - 1) / perRow;
}

void MainScreen::setCameraScale (int step)
{
    // Clamped rather than asserted: this arrives from a settings file, which a
    // future version may have written a larger number into.
    const int clamped = juce::jlimit (0, kCameraScaleSteps - 1, step);

    if (clamped == cameraScale)
        return;

    cameraScale = clamped;

    // The ends of the range are the one thing the arrows must say for
    // themselves -- a button that keeps accepting clicks and changes nothing
    // reads as broken.
    cameraSmallerButton.setEnabled (cameraScale > 0);
    cameraLargerButton.setEnabled (cameraScale < kCameraScaleSteps - 1);

    resized();
}

void MainScreen::setCameraTiles (const std::vector<CameraTile>& tiles)
{
    std::vector<std::string> ids;
    ids.reserve (tiles.size());
    for (const auto& tile : tiles)
        ids.push_back (tile.id);

    // Called from the UI tick, so it must be free to run every frame. Only an
    // actual change to the set of switched-on cameras rebuilds anything --
    // tearing a viewer down and remaking it 60 times a second would flicker and
    // churn the device for no reason.
    if (ids == lastTileIds)
        return;

    lastTileIds = std::move (ids);
    cameraViews.clear();

    for (const auto& tile : tiles)
    {
        CameraView view;
        view.id = tile.id;

        if (makeViewer)
            view.viewer = makeViewer (tile.id);

        if (view.viewer != nullptr)
        {
            addAndMakeVisible (*view.viewer);
        }
        else
        {
            // A camera that would not open gets a well saying so, never an
            // empty rectangle -- and on Linux, where JUCE has no CameraDevice
            // at all, this is the honest thing to show rather than a black box.
            view.placeholder = std::make_unique<juce::Label>();
            view.placeholder->setText ("No picture from this camera.", juce::dontSendNotification);
            view.placeholder->setJustificationType (juce::Justification::centred);
            view.placeholder->setFont (juce::Font (12.0f));
            view.placeholder->setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
            addAndMakeVisible (*view.placeholder);
        }

        // Whose picture this is. Four identical webcams are the same problem
        // §14.6 solves for microphones, and the answer is the same: put the
        // name on the thing.
        view.caption = std::make_unique<juce::Label>();
        view.caption->setText (tile.displayName, juce::dontSendNotification);
        view.caption->setJustificationType (juce::Justification::centred);
        view.caption->setFont (juce::Font (12.0f));
        view.caption->setColour (juce::Label::textColourId, AppLookAndFeel::secondary);
        addAndMakeVisible (*view.caption);

        cameraViews.push_back (std::move (view));
    }

    resized();
}

void MainScreen::releaseCameraViews()
{
    if (cameraViews.empty())
        return;

    cameraViews.clear();

    // Cleared too, so the next setCameraTiles() rebuilds rather than deciding
    // nothing has changed and leaving the row empty.
    lastTileIds.clear();
    resized();
}

int MainScreen::cameraRowHeight() const
{
    // Zero when nothing is switched on, which is what keeps an audio-only rig
    // laid out exactly as it was before the pictures arrived.
    if (cameraViews.empty())
        return 0;

    // Measured against the width the screen actually has, because that decides
    // how many tiles fit across and therefore how many rows they wrap onto. Its
    // own width is the honest answer; before the first resize() there is none,
    // so fall back to the window width the owner opens at.
    const int available = juce::jmax (1, (getWidth() > 0 ? getWidth() : 720) - 32);
    const int tileWidth = juce::jmin (cameraTileWidthAt (cameraScale), juce::jmax (72, available));
    const int rows = cameraRowsNeeded (tileWidth, available);
    const int cellHeight = tileHeightFor (tileWidth) + kCameraCaptionHeight;

    return kCameraControlHeight + 4
           + rows * cellHeight + (rows - 1) * kCameraTileGap
           + kCameraRowGap;
}

int MainScreen::getRequiredHeight() const
{
    // The channel-strip row plus every fixed row resized() lays out beneath it,
    // and the margins around the lot -- plus the camera row when there is one.
    // Adding it here rather than letting it overflow is what keeps the record
    // button on screen once the pictures are there: the owner sizes the window
    // from this, and a row that did not declare itself would simply push the
    // button below the fold.
    return 522 + 24 + cameraRowHeight();
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

    // The pictures sit above the levels: the shot is what you look at, the
    // meters are what you glance at, and putting them in that order keeps the
    // record button where it has always been at the bottom.
    const bool haveCameras = ! cameraViews.empty();

    cameraSizeLabel.setVisible (haveCameras);
    cameraSmallerButton.setVisible (haveCameras);
    cameraLargerButton.setVisible (haveCameras);

    if (haveCameras)
    {
        auto cameraArea = area.removeFromTop (cameraRowHeight() - kCameraRowGap);

        // The arrows sit on their own line above the pictures, right-aligned so
        // they do not wander as the tiles change size underneath them.
        auto controls = cameraArea.removeFromTop (kCameraControlHeight);
        cameraLargerButton.setBounds (controls.removeFromRight (28).reduced (1));
        controls.removeFromRight (4);
        cameraSmallerButton.setBounds (controls.removeFromRight (28).reduced (1));
        controls.removeFromRight (6);
        cameraSizeLabel.setBounds (controls.removeFromRight (80));

        cameraArea.removeFromTop (4);

        const int available = cameraArea.getWidth();
        const int tileWidth = juce::jmin (cameraTileWidthAt (cameraScale), juce::jmax (72, available));
        const int tileHeight = tileHeightFor (tileWidth);
        const int cellHeight = tileHeight + kCameraCaptionHeight;

        // Wrap rather than shrink. Asking for a bigger picture on a four-camera
        // rig has to give you one; squeezing them all onto one row instead
        // would make the arrows do nothing precisely when they are wanted.
        const int perRow = juce::jmax (1, (available + kCameraTileGap) / (tileWidth + kCameraTileGap));

        int index = 0;
        for (auto& view : cameraViews)
        {
            if (index % perRow == 0)
            {
                if (index > 0)
                    cameraArea.removeFromTop (kCameraTileGap);

                const int inThisRow = juce::jmin (perRow, static_cast<int> (cameraViews.size()) - index);
                const int rowWidth = inThisRow * tileWidth + (inThisRow - 1) * kCameraTileGap;

                auto row = cameraArea.removeFromTop (cellHeight);

                // Centred as a block, like the channel strips: two cameras sit
                // in the middle rather than hugging the left edge.
                if (rowWidth < available)
                    row.removeFromLeft ((available - rowWidth) / 2);

                // Stashed for the tiles in this row to consume.
                cameraRowScratch = row;
            }
            else
            {
                cameraRowScratch.removeFromLeft (kCameraTileGap);
            }

            auto tile = cameraRowScratch.removeFromLeft (tileWidth);
            auto caption = tile.removeFromBottom (kCameraCaptionHeight);

            if (view.caption != nullptr)
                view.caption->setBounds (caption);

            if (view.viewer != nullptr)
                view.viewer->setBounds (tile);
            else if (view.placeholder != nullptr)
                view.placeholder->setBounds (tile);

            ++index;
        }

        area.removeFromTop (kCameraRowGap);
    }

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
