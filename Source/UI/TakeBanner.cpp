#include "TakeBanner.h"
#include "AppLookAndFeel.h"

namespace mma {

TakeBanner::TakeBanner()
{
    // Never in the way: clicks and keys go straight through to whatever is
    // behind it.
    setInterceptsMouseClicks (false, false);
    setWantsKeyboardFocus (false);
    setAccessible (true);
}

TakeBanner::~TakeBanner()
{
    stopTimer();
}

void TakeBanner::show (Kind newKind, const juce::String& newTitle, const juce::String& newDetail)
{
    kind = newKind;
    title = newTitle;
    detail = newDetail;
    shownAtMs = juce::Time::getMillisecondCounterHiRes();

    setTitle (title + (detail.isNotEmpty() ? ". " + detail : juce::String()));
    setVisible (true);
    toFront (false);
    startTimerHz (30);
    repaint();
}

bool TakeBanner::litNow() const
{
    if (reducedMotion)
        return true;

    const auto elapsed = juce::Time::getMillisecondCounterHiRes() - shownAtMs;
    return (static_cast<int> (elapsed / kFlashMs) % 2) == 0;
}

void TakeBanner::timerCallback()
{
    const auto elapsed = juce::Time::getMillisecondCounterHiRes() - shownAtMs;

    if (elapsed >= kHoldMs)
    {
        stopTimer();
        setVisible (false);
        return;
    }

    // A card raised behind this in the meantime must not cover it.
    toFront (false);
    repaint();
}

void TakeBanner::paint (juce::Graphics& g)
{
    const auto colour = kind == Kind::Started ? AppLookAndFeel::danger : AppLookAndFeel::accent;
    const bool lit = litNow();

    // Lit: the whole window in the colour. Unlit: near-black with a thick
    // border in the colour, so even the off phase is unmistakably a banner.
    g.fillAll (lit ? colour : AppLookAndFeel::background);

    if (! lit)
    {
        g.setColour (colour);
        g.drawRect (getLocalBounds(), 14);
    }

    auto area = getLocalBounds().reduced (24);
    const auto textColour = lit ? AppLookAndFeel::background : colour;

    // As big as the window allows, and never small.
    const float titleSize = juce::jlimit (40.0f, 120.0f, static_cast<float> (area.getHeight()) / 4.0f);
    const float detailSize = juce::jlimit (18.0f, 40.0f, titleSize * 0.32f);

    auto titleArea = area.removeFromTop (juce::roundToInt (area.getHeight() * 0.62f));
    g.setColour (textColour);
    g.setFont (juce::Font (titleSize, juce::Font::bold));
    g.drawFittedText (title, titleArea, juce::Justification::centred, 2, 0.8f);

    if (detail.isNotEmpty())
    {
        g.setFont (juce::Font (detailSize, juce::Font::bold));
        g.drawFittedText (detail, area, juce::Justification::centredTop, 2, 0.8f);
    }
}

} // namespace mma
