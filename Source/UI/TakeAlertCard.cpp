#include "TakeAlertCard.h"
#include "AppLookAndFeel.h"

namespace mma {

TakeAlertCard::TakeAlertCard()
{
    setHeading ("Something changed mid-take.",
                "The recording is still running. Here is what happened, newest last.");

    // Carrying on is the safe default and gets the accent; stopping is the
    // drastic one and is red, so nobody hits it by reflex.
    keepButton.setColour (juce::TextButton::buttonColourId, AppLookAndFeel::accent);
    keepButton.setColour (juce::TextButton::textColourOffId, AppLookAndFeel::background);
    keepButton.onClick = [this] { if (onKeepRecording) onKeepRecording(); };
    addAndMakeVisible (keepButton);

    stopButton.setColour (juce::TextButton::buttonColourId, AppLookAndFeel::danger);
    stopButton.setColour (juce::TextButton::textColourOffId, AppLookAndFeel::background);
    stopButton.onClick = [this] { if (onStopRecording) onStopRecording(); };
    addAndMakeVisible (stopButton);
}

TakeAlertCard::~TakeAlertCard() = default;

void TakeAlertCard::addAlert (const juce::String& whenInTake, const juce::String& message, bool recovery)
{
    // Oldest lines fall off the top: the card is for what just happened, and
    // six lines is already more than anyone reads in a hurry.
    while (rows.size() >= static_cast<size_t> (kMaxRows))
        rows.erase (rows.begin());

    Row row;
    row.when = std::make_unique<juce::Label>();
    row.when->setText (whenInTake, juce::dontSendNotification);
    row.when->setFont (juce::Font (11.0f));
    row.when->setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
    row.when->setJustificationType (juce::Justification::topLeft);
    addAndMakeVisible (*row.when);

    row.text = std::make_unique<juce::Label>();
    styleBody (*row.text, recovery ? AppLookAndFeel::secondary : AppLookAndFeel::warning);
    row.text->setText (message, juce::dontSendNotification);
    addAndMakeVisible (*row.text);

    rows.push_back (std::move (row));
    resized();
}

void TakeAlertCard::clear()
{
    rows.clear();
    setSevere (false, false);
    resized();
}

void TakeAlertCard::setSevere (bool severe, bool takeStopped)
{
    if (severe)
    {
        setHeading (takeStopped ? "Recording failed." : "Recording is in trouble.",
                    takeStopped ? "The take was stopped. Nothing after this point was recorded."
                                : "The take is still running, but read this before trusting it.");
        setHeadingColour (AppLookAndFeel::danger);
        keepButton.setVisible (! takeStopped);
        stopButton.setButtonText (takeStopped ? "OK" : "Stop recording");
    }
    else
    {
        setHeading ("Something changed mid-take.",
                    "The recording is still running. Here is what happened, newest last.");
        setHeadingColour (AppLookAndFeel::bone);
        keepButton.setVisible (true);
        stopButton.setButtonText ("Stop recording");
    }

    resized();
}

bool TakeAlertCard::keyPressed (const juce::KeyPress& key)
{
    // Escape and Return both mean "I've seen it, carry on". Nothing on the
    // keyboard stops a take from here.
    if (key == juce::KeyPress::escapeKey || key == juce::KeyPress::returnKey)
    {
        if (onKeepRecording)
            onKeepRecording();

        return true;
    }

    return false;
}

int TakeAlertCard::getContentHeight() const
{
    return static_cast<int> (rows.size()) * kRowHeight + 14 + kButtonHeight;
}

void TakeAlertCard::layOutContent (juce::Rectangle<int> area)
{
    for (auto& row : rows)
    {
        auto line = area.removeFromTop (kRowHeight);
        row.when->setBounds (line.removeFromLeft (64));
        row.text->setBounds (line);
    }

    area.removeFromTop (14);
    auto buttons = area.removeFromTop (kButtonHeight);
    stopButton.setBounds (buttons.removeFromLeft (150));
    keepButton.setBounds (buttons.removeFromRight (juce::jmin (170, buttons.getWidth())));
}

} // namespace mma
