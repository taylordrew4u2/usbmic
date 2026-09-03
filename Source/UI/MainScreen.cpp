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

    // The masthead. Letter-spaced by hand, because a wordmark is the one piece
    // of type on this screen that is a picture of a word rather than a word.
    brandLabel.setText ("S O B S T A G E", juce::dontSendNotification);
    brandLabel.setFont (juce::Font (16.0f, juce::Font::bold));
    brandLabel.setColour (juce::Label::textColourId, AppLookAndFeel::bone);
    brandLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (brandLabel);

    taglineLabel.setText ("let's give these tears a stage", juce::dontSendNotification);
    taglineLabel.setFont (juce::Font (13.0f));
    taglineLabel.setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
    taglineLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (taglineLabel);

    // Small, wide-tracked, tertiary: a heading that labels a group without
    // competing with anything inside it.
    for (auto* l : { &cameraSectionLabel, &micSectionLabel })
    {
        l->setFont (juce::Font (11.0f, juce::Font::bold));
        l->setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
        l->setJustificationType (juce::Justification::centredLeft);
        addChildComponent (*l);
    }

    cameraSectionLabel.setText ("C A M E R A", juce::dontSendNotification);
    micSectionLabel.setText ("M I C R O P H O N E S", juce::dontSendNotification);
    micSectionLabel.setVisible (true);

    fullPreviewToggle.onClick = [this]
    { if (onFullPreviewToggled) onFullPreviewToggled (fullPreviewToggle.getToggleState()); };
    addChildComponent (fullPreviewToggle);

    // The door to the camera panel, on the row it belongs to. The old
    // "Cameras" button at the foot of the screen stays for the rig that has
    // none switched on yet -- with no pictures there is no row for this to sit
    // on, and a user with no camera still has to be able to find the panel.
    addCameraButton.setColour (juce::TextButton::buttonColourId, AppLookAndFeel::surfaceHigh);
    addCameraButton.onClick = [this] { if (onCamerasClicked) onCamerasClicked(); };
    addChildComponent (addCameraButton);
}

void MainScreen::paintBrandMark (juce::Graphics& g, juce::Rectangle<float> b) const
{
    // The same four shapes as Tools/make_icon.py, at the size a title bar has.
    const auto cx = b.getCentreX();
    const auto cy = b.getCentreY();
    const auto r = juce::jmin (b.getWidth(), b.getHeight()) * 0.46f;

    g.setColour (AppLookAndFeel::bone);
    g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, juce::jmax (1.2f, r * 0.14f));

    const float eyeR = r * 0.115f;
    const float eyeDx = r * 0.37f;
    const float eyeY = cy - r * 0.22f;
    for (float sx : { -1.0f, 1.0f })
        g.fillEllipse (cx + sx * eyeDx - eyeR, eyeY - eyeR, eyeR * 2.0f, eyeR * 2.0f);

    // The frown: an arc of a circle centred below the face, so what shows is
    // its top edge. Centred above, the identical arc reads as a smile.
    juce::Path mouth;
    const float mouthR = r * 0.45f;
    const float mouthCy = cy + r * 0.70f;
    mouth.addCentredArc (cx, mouthCy, mouthR, mouthR, 0.0f,
                         juce::MathConstants<float>::pi * 1.28f,
                         juce::MathConstants<float>::pi * 1.72f, true);
    g.strokePath (mouth, juce::PathStrokeType (juce::jmax (1.0f, r * 0.12f)));

    // The tear, and the only saturated thing in the mark.
    g.setColour (AppLookAndFeel::accent);
    const float dropR = r * 0.12f;
    const float tx = cx - eyeDx;
    const float ty = eyeY + r * 0.50f;
    g.fillEllipse (tx - dropR, ty - dropR, dropR * 2.0f, dropR * 2.0f);

    juce::Path drop;
    drop.addTriangle (tx, ty - dropR * 2.6f, tx - dropR, ty, tx + dropR, ty);
    g.fillPath (drop);
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
    // Every band resized() lays out, added up, rather than a constant left over
    // from a layout this no longer is. The old 522 was measured against tall
    // channel strips and a centred status stack; against the current screen it
    // left a band of empty background between the advice line and the footer
    // roughly the height of the record button.
    constexpr int kMargins       = 32;   // 16 top and bottom
    constexpr int kHeader        = 36 + 18;
    constexpr int kMicHeading    = 16 + 6;
    constexpr int kStripHeight   = 40;
    constexpr int kStripGap      = 12;
    constexpr int kActionRow     = 16 + 52;
    constexpr int kStatusLines   = 18 + 20 + 20;  // files, monitor problem, advice
    constexpr int kFooter        = 34;

    // MIX is laid out with the microphones, so it counts towards the wrap.
    const int cells = juce::jmax (1, skullMeters.size() + 1);
    const int available = juce::jmax (1, (getWidth() > 0 ? getWidth() : 720) - 32);
    const int perRow = juce::jlimit (1, cells, (available + kStripGap) / (190 + kStripGap));
    const int rows = (cells + perRow - 1) / perRow;

    return kMargins + kHeader + cameraRowHeight() + kMicHeading
         + rows * kStripHeight + (rows - 1) * kStripGap
         + kActionRow + kStatusLines + kFooter;
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

    if (! brandMarkBounds.isEmpty())
        paintBrandMark (g, brandMarkBounds.toFloat());

    // A hairline under the masthead, so the title reads as a masthead rather
    // than as the first row of the content beneath it.
    if (! brandMarkBounds.isEmpty())
    {
        g.setColour (AppLookAndFeel::outline);
        const int y = brandMarkBounds.getBottom() + 10;
        g.fillRect (16, y, getWidth() - 32, 1);
    }

    // §9.3: the picture says it is recording in a word, not only in a colour.
    // A red dot alone is the one indicator a colour-blind user cannot read, and
    // it is the one that matters most.
    for (const auto& badge : cameraRecBadges)
    {
        g.setColour (AppLookAndFeel::background.withAlpha (0.72f));
        g.fillRoundedRectangle (badge.toFloat(), 4.0f);

        auto dot = badge.toFloat().removeFromLeft (16.0f);
        g.setColour (AppLookAndFeel::danger);
        g.fillEllipse (dot.withSizeKeepingCentre (7.0f, 7.0f));

        g.setFont (juce::Font (10.0f, juce::Font::bold));
        g.drawText ("REC", badge.withTrimmedLeft (16), juce::Justification::centredLeft);
    }
}

void MainScreen::resized()
{
    auto area = getLocalBounds().reduced (16);
    cameraRecBadges.clear();

    // The masthead: the mark, the name, the tagline, and the one door out.
    // Settings moved up here from the foot of the screen because it is a
    // destination rather than a control -- it belongs with the title, not in
    // the row where the levels are being set.
    {
        auto header = area.removeFromTop (36);

        brandMarkBounds = header.removeFromLeft (28).withSizeKeepingCentre (26, 26);
        header.removeFromLeft (10);

        advancedButton.setBounds (header.removeFromRight (96).reduced (0, 3));
        header.removeFromRight (8);

        brandLabel.setBounds (header.removeFromLeft (juce::jmin (128, header.getWidth())));
        header.removeFromLeft (10);
        taglineLabel.setBounds (header);

        area.removeFromTop (18);
    }

    // The pictures sit above the levels: the shot is what you look at, the
    // meters are what you glance at, and putting them in that order keeps the
    // record button where it has always been at the bottom.
    const bool haveCameras = ! cameraViews.empty();

    cameraSizeLabel.setVisible (haveCameras);
    cameraSmallerButton.setVisible (haveCameras);
    cameraLargerButton.setVisible (haveCameras);
    cameraSectionLabel.setVisible (haveCameras);
    fullPreviewToggle.setVisible (haveCameras);
    addCameraButton.setVisible (haveCameras);

    if (haveCameras)
    {
        auto cameraArea = area.removeFromTop (cameraRowHeight() - kCameraRowGap);

        // One row carrying the heading and everything that acts on the
        // pictures: what they are, how big, how good the preview is, and how to
        // add another. Grouped here rather than scattered down the screen,
        // because they are one job.
        auto controls = cameraArea.removeFromTop (kCameraControlHeight);
        cameraSectionLabel.setBounds (controls.removeFromLeft (110));

        addCameraButton.setBounds (controls.removeFromRight (108).reduced (0, 1));
        controls.removeFromRight (10);
        fullPreviewToggle.setBounds (controls.removeFromRight (108));
        controls.removeFromRight (10);
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

            // Over the top-left of the picture while a take is running, where a
            // camera's own tally light sits. paint() draws it; the viewer is a
            // child component, so a badge drawn under it would be invisible --
            // this is why it is a rectangle collected here rather than a
            // component added to the tile.
            if (recording)
                cameraRecBadges.push_back (tile.reduced (8).removeFromTop (18).removeFromLeft (54));

            ++index;
        }

        area.removeFromTop (kCameraRowGap);
    }

    // The levels, as a row of horizontal strips rather than a rank of tall
    // channels. The pictures are what take the height now, and a level being
    // watched out of the corner of an eye is better served by a wide track
    // than by a tall one: the same information, in the shape the space is.
    //
    // MIX rides in the same row as one more strip, because it is one more
    // level -- it was a full-width bar of its own, which said it was a
    // different kind of thing than the channels it is the sum of.
    micSectionLabel.setBounds (area.removeFromTop (16));
    area.removeFromTop (6);

    if (skullMeters.isEmpty())
    {
        noMicsLabel.setBounds (area.removeFromTop (56));
        mixBar.setBounds ({});
    }
    else
    {
        constexpr int kStripHeight = 40;
        constexpr int kStripGap    = 12;
        constexpr int kMinStripWidth = 190;

        // MIX is laid out with them, so the wrap has to count it.
        const int cells = skullMeters.size() + 1;
        const int available = area.getWidth();

        const int perRow = juce::jlimit (1, cells,
                                         (available + kStripGap) / (kMinStripWidth + kStripGap));
        const int rows = (cells + perRow - 1) / perRow;

        auto stripArea = area.removeFromTop (rows * kStripHeight + (rows - 1) * kStripGap);

        juce::Rectangle<int> row;
        for (int i = 0; i < cells; ++i)
        {
            if (i % perRow == 0)
            {
                if (i > 0)
                    stripArea.removeFromTop (kStripGap);

                row = stripArea.removeFromTop (kStripHeight);
            }
            else
            {
                row.removeFromLeft (kStripGap);
            }

            // One width for every cell on the screen, taken from a full row.
            // Sizing the last row's cells to what is left instead stretched a
            // lone MIX across the entire window, which said it was a different
            // kind of thing than the channels it is the sum of.
            const int cellWidth = (available - (perRow - 1) * kStripGap) / perRow;

            auto cell = row.removeFromLeft (juce::jmin (cellWidth, row.getWidth()));

            if (i < skullMeters.size())
            {
                skullMeters[i]->setOrientation (SkullMeterComponent::Orientation::Strip);
                skullMeters[i]->setBounds (cell);
            }
            else
            {
                mixBar.setBounds (cell);
            }
        }
    }

    area.removeFromTop (16);

    // Side by side: naming the take and starting it are one action, and
    // stacking them put 8px of nothing between a field and the button that
    // consumes it while the eye travelled the full width twice.
    {
        auto actionRow = area.removeFromTop (52);
        const int buttonWidth = juce::jlimit (200, 380, actionRow.getWidth() / 2);

        recordButton.setBounds (actionRow.removeFromRight (buttonWidth));
        actionRow.removeFromRight (14);
        sessionNameEditor.setBounds (actionRow.withSizeKeepingCentre (actionRow.getWidth(), 40));
    }

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

    // Elapsed, remaining and the destination all live in the footer now, so
    // nothing is placed here -- the record button is followed straight by the
    // lines that qualify it.
    filesSavingLabel.setBounds (area.removeFromTop (18));
    monitorProblemLabel.setBounds (area.removeFromTop (20));
    adviceLabel.setBounds (area.removeFromTop (20));

    // The footer: what the disk knows on the left, what the ears want on the
    // right. Both are ambient -- neither is something anyone comes to this
    // screen to do -- so they share one quiet row at the bottom rather than
    // taking a band of the middle each.
    auto bottomRow = area.removeFromBottom (34);

    muteButton.setBounds (bottomRow.removeFromRight (76));
    bottomRow.removeFromRight (8);
    volumeSlider.setBounds (bottomRow.removeFromRight (220));
    bottomRow.removeFromRight (16);

    // The door to the cameras only earns a place down here when there is no
    // camera row carrying "+ Add camera" -- otherwise the same door appears
    // twice on one screen.
    if (! haveCameras)
    {
        camerasButton.setBounds (bottomRow.removeFromRight (110).reduced (0, 2));
        bottomRow.removeFromRight (10);
    }
    else
    {
        camerasButton.setBounds ({});
    }

    remainingLabel.setJustificationType (juce::Justification::centredLeft);
    saveLocationLabel.setJustificationType (juce::Justification::centredLeft);
    elapsedLabel.setJustificationType (juce::Justification::centredLeft);

    // Elapsed only appears while a take is running, and takes the space from
    // the destination rather than from the capacity figure: during a take, how
    // long it has been going matters more than where it is going, which has
    // not changed since it started.
    if (recording)
    {
        elapsedLabel.setBounds (bottomRow.removeFromLeft (150));
        remainingLabel.setBounds (bottomRow.removeFromLeft (190));
        saveLocationLabel.setBounds (bottomRow);
    }
    else
    {
        elapsedLabel.setBounds ({});
        remainingLabel.setBounds (bottomRow.removeFromLeft (juce::jmin (200, bottomRow.getWidth() / 2)));
        saveLocationLabel.setBounds (bottomRow);
    }
}

} // namespace mma
