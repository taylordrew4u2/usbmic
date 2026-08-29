#include "MainScreen.h"

namespace mma {

const juce::Colour MainScreen::kBackground { 0xFF16110F };

MainScreen::MainScreen()
{
    addAndMakeVisible (mixBar);

    recordButton.onClick = [this] { if (onRecordButtonClicked) onRecordButtonClicked(); };
    addAndMakeVisible (recordButton);

    elapsedLabel.setJustificationType (juce::Justification::centred);
    remainingLabel.setJustificationType (juce::Justification::centred);
    saveLocationLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (elapsedLabel);
    addAndMakeVisible (remainingLabel);
    addAndMakeVisible (saveLocationLabel);

    noMicsLabel.setText ("Plug in a microphone to get started.", juce::dontSendNotification);
    noMicsLabel.setJustificationType (juce::Justification::centred);
    noMicsLabel.setVisible (false);
    addAndMakeVisible (noMicsLabel);

    adviceLabel.setJustificationType (juce::Justification::centred);
    adviceLabel.setVisible (false);
    addAndMakeVisible (adviceLabel);

    monitorProblemLabel.setJustificationType (juce::Justification::centred);
    monitorProblemLabel.setColour (juce::Label::textColourId, juce::Colours::orange);
    monitorProblemLabel.setVisible (false);
    addAndMakeVisible (monitorProblemLabel);

    disabledReasonLabel.setJustificationType (juce::Justification::centred);
    disabledReasonLabel.setVisible (false);
    addAndMakeVisible (disabledReasonLabel);

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
    return 504 + 24;
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
    recording = isRecording;
    // §10.4/§10.6: buttons say what happens. "Start recording" -> "Recording."
    recordButton.setButtonText (recording ? "Recording. Tap to stop." : "Start recording");
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
    disabledReasonLabel.setVisible (! enabled);
    disabledReasonLabel.setText (disabledReason, juce::dontSendNotification);
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
        constexpr int kStripWidth = 84;
        const int available = skullRow.getWidth();
        const int wanted = kStripWidth * juce::jmax (1, skullMeters.size());
        const int meterWidth = wanted <= available
                                   ? kStripWidth
                                   : juce::jmax (44, available / juce::jmax (1, skullMeters.size()));

        // Centred as a block, so a rig of two sits in the middle rather than
        // hugging the left edge.
        if (wanted < available)
            skullRow.removeFromLeft ((available - wanted) / 2);
        for (auto* meter : skullMeters)
            meter->setBounds (skullRow.removeFromLeft (meterWidth).reduced (2, 0));
    }

    mixBar.setBounds (area.removeFromTop (28));
    area.removeFromTop (8);

    sessionNameEditor.setBounds (area.removeFromTop (26).reduced (area.getWidth() / 5, 0));
    area.removeFromTop (4);
    recordButton.setBounds (area.removeFromTop (56));
    disabledReasonLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (8);

    auto statusRow = area.removeFromTop (24);
    elapsedLabel.setBounds (statusRow.removeFromLeft (statusRow.getWidth() / 2));
    remainingLabel.setBounds (statusRow);

    saveLocationLabel.setBounds (area.removeFromTop (20));
    monitorProblemLabel.setBounds (area.removeFromTop (20));
    adviceLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (8);

    auto bottomRow = area.removeFromTop (32);
    volumeSlider.setBounds (bottomRow.removeFromLeft (bottomRow.getWidth() * 2 / 3));
    muteButton.setBounds (bottomRow.removeFromLeft (bottomRow.getWidth() / 2));
    advancedButton.setBounds (bottomRow);
}

} // namespace mma
