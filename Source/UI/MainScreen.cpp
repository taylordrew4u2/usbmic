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

    disabledReasonLabel.setJustificationType (juce::Justification::centred);
    disabledReasonLabel.setVisible (false);
    addAndMakeVisible (disabledReasonLabel);

    volumeSlider.setRange (0.0, 100.0, 1.0);
    volumeSlider.setValue (70.0); // §5.1 default
    volumeSlider.onValueChange = [this] { if (onVolumeChanged) onVolumeChanged (volumeSlider.getValue()); };
    addAndMakeVisible (volumeSlider);

    muteButton.onClick = [this] { if (onMuteToggled) onMuteToggled(); };
    addAndMakeVisible (muteButton);

    advancedButton.onClick = [this] { if (onAdvancedClicked) onAdvancedClicked(); };
    addAndMakeVisible (advancedButton);
}

MainScreen::~MainScreen() = default;

void MainScreen::setMicCount (int count)
{
    skullMeters.clear();
    for (int i = 0; i < count; ++i)
    {
        auto* meter = new SkullMeterComponent();
        addAndMakeVisible (meter);
        skullMeters.add (meter);
    }
    setNoMicsMessage (count == 0);
    resized();
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

    auto skullRow = area.removeFromTop (160);
    if (skullMeters.isEmpty())
    {
        noMicsLabel.setBounds (skullRow);
    }
    else
    {
        const int meterWidth = juce::jmax (60, skullRow.getWidth() / juce::jmax (1, skullMeters.size()));
        for (auto* meter : skullMeters)
            meter->setBounds (skullRow.removeFromLeft (meterWidth).reduced (4));
    }

    mixBar.setBounds (area.removeFromTop (28));
    area.removeFromTop (8);

    recordButton.setBounds (area.removeFromTop (56));
    disabledReasonLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (8);

    auto statusRow = area.removeFromTop (24);
    elapsedLabel.setBounds (statusRow.removeFromLeft (statusRow.getWidth() / 2));
    remainingLabel.setBounds (statusRow);

    saveLocationLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (8);

    auto bottomRow = area.removeFromTop (32);
    volumeSlider.setBounds (bottomRow.removeFromLeft (bottomRow.getWidth() * 2 / 3));
    muteButton.setBounds (bottomRow.removeFromLeft (bottomRow.getWidth() / 2));
    advancedButton.setBounds (bottomRow);
}

} // namespace mma
