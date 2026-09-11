#include "TakeAlertCard.h"
#include "AppLookAndFeel.h"

namespace mma {

TakeAlertCard::TakeAlertCard()
{
    refreshCalmHeading();

    // Carrying on is the safe default and gets the accent; stopping is the
    // drastic one and is outlined in red, so the only filled button on the
    // card is the one nobody regrets pressing.
    keepButton.setColour (juce::TextButton::buttonColourId, AppLookAndFeel::accent);
    keepButton.setColour (juce::TextButton::textColourOffId, AppLookAndFeel::background);
    keepButton.onClick = [this] { if (onKeepRecording) onKeepRecording(); };
    addAndMakeVisible (keepButton);

    stopButton.setColour (juce::TextButton::buttonColourId, AppLookAndFeel::surfaceHigh);
    stopButton.setColour (juce::TextButton::textColourOffId, AppLookAndFeel::danger);
    stopButton.onClick = [this] { if (onStopRecording) onStopRecording(); };
    addAndMakeVisible (stopButton);
}

TakeAlertCard::~TakeAlertCard() = default;

juce::Colour TakeAlertCard::colourFor (Tone tone)
{
    switch (tone)
    {
        case Tone::NeedsYou: return AppLookAndFeel::danger;
        case Tone::Watch:    return AppLookAndFeel::warning;
        case Tone::Fixed:    return AppLookAndFeel::meterLow;
        case Tone::Quiet:    break;
    }

    return AppLookAndFeel::dimmedOutline;
}

void TakeAlertCard::addAlert (const juce::String& whenInTake, const juce::String& message, Tone tone)
{
    // Oldest lines fall off the bottom: the card is for what just happened,
    // and six lines is already more than anyone reads in a hurry. What falls
    // off is counted, not forgotten.
    while (rows.size() >= static_cast<size_t> (kMaxRows))
    {
        rows.pop_back();
        ++rowsDropped;
    }

    Row row;
    row.tone = tone;

    row.when = std::make_unique<juce::Label>();
    row.when->setText (whenInTake, juce::dontSendNotification);
    row.when->setFont (juce::Font (11.0f));
    row.when->setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
    row.when->setJustificationType (juce::Justification::topRight);
    addAndMakeVisible (*row.when);

    row.text = std::make_unique<juce::Label>();
    styleBody (*row.text, tone == Tone::Quiet || tone == Tone::Fixed
                              ? AppLookAndFeel::secondary
                              : AppLookAndFeel::bone);
    row.text->setText (message, juce::dontSendNotification);
    addAndMakeVisible (*row.text);

    // Newest at the top. Reading downwards used to mean reading the oldest
    // news first, on a card that is only up because something just happened.
    rows.insert (rows.begin(), std::move (row));

    refreshOverflowLabel();

    if (! severeHeading)
        refreshCalmHeading();

    resized();
}

void TakeAlertCard::clear()
{
    rows.clear();
    rowsDropped = 0;
    refreshOverflowLabel();
    setSevere (false, false);
    resized();
}

void TakeAlertCard::refreshOverflowLabel()
{
    if (rowsDropped <= 0)
    {
        overflowLabel.reset();
        return;
    }

    if (overflowLabel == nullptr)
    {
        overflowLabel = std::make_unique<juce::Label>();
        overflowLabel->setFont (juce::Font (12.0f));
        overflowLabel->setColour (juce::Label::textColourId, AppLookAndFeel::tertiary);
        overflowLabel->setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (*overflowLabel);
    }

    overflowLabel->setText (rowsDropped == 1
                                ? "1 earlier event is in this take's log."
                                : juce::String (rowsDropped) + " earlier events are in this take's log.",
                            juce::dontSendNotification);
}

void TakeAlertCard::refreshCalmHeading()
{
    // The heading answers the only question anyone has mid-take: is my
    // recording safe? "Something changed mid-take." answered a different one.
    bool anyNeedsYou = false;

    for (const auto& row : rows)
        anyNeedsYou = anyNeedsYou || row.tone == Tone::NeedsYou;

    if (anyNeedsYou)
        setHeading ("Your take is still running.",
                    "One thing below still needs you. The recording has not stopped.");
    else
        setHeading ("Your take is safe.",
                    "Here is what happened while you were rolling, newest first.");

    setHeadingColour (AppLookAndFeel::bone);
}

void TakeAlertCard::setSevere (bool severe, bool takeStopped)
{
    severeHeading = severe;

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
        refreshCalmHeading();
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

void TakeAlertCard::paint (juce::Graphics& g)
{
    ModalCard::paint (g);

    // The severity bar. Colour is the whole point of it: a microphone that
    // has gone and a camera that has gone are not the same emergency.
    for (const auto& row : rows)
    {
        if (row.bar.isEmpty())
            continue;

        g.setColour (colourFor (row.tone));
        g.fillRoundedRectangle (row.bar.toFloat(), static_cast<float> (kBarWidth) * 0.5f);
    }
}

int TakeAlertCard::getContentHeight() const
{
    return static_cast<int> (rows.size()) * kRowHeight
           + (rowsDropped > 0 ? kOverflowHeight : 0)
           + 14 + kButtonHeight;
}

void TakeAlertCard::layOutContent (juce::Rectangle<int> area)
{
    for (auto& row : rows)
    {
        auto line = area.removeFromTop (kRowHeight);
        row.bar = line.removeFromLeft (kBarWidth).withTrimmedBottom (14);
        line.removeFromLeft (12);
        row.when->setBounds (line.removeFromRight (kWhenWidth));
        row.text->setBounds (line.withTrimmedRight (10));
    }

    if (overflowLabel != nullptr)
        overflowLabel->setBounds (area.removeFromTop (kOverflowHeight)
                                      .withTrimmedLeft (kBarWidth + 12));

    area.removeFromTop (14);
    auto buttons = area.removeFromTop (kButtonHeight);
    stopButton.setBounds (buttons.removeFromLeft (150));
    keepButton.setBounds (buttons.removeFromRight (juce::jmin (170, buttons.getWidth())));
}

} // namespace mma
