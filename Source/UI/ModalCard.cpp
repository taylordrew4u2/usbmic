#include "ModalCard.h"
#include "AppLookAndFeel.h"

namespace mma {

ModalCard::ModalCard()
{
    // The card is the only thing on screen that matters while it is up, so it
    // takes the keys too -- Return starts, Escape backs out, and neither should
    // reach the main screen's spacebar mute behind it.
    setWantsKeyboardFocus (true);
    setInterceptsMouseClicks (true, true);

    headingLabel.setFont (juce::Font (20.0f, juce::Font::bold));
    headingLabel.setColour (juce::Label::textColourId, AppLookAndFeel::bone);
    addAndMakeVisible (headingLabel);

    styleBody (subheadingLabel, AppLookAndFeel::secondary);
    addAndMakeVisible (subheadingLabel);
}

ModalCard::~ModalCard() = default;

void ModalCard::styleBody (juce::Label& label, const juce::Colour& colour)
{
    label.setFont (juce::Font (13.0f));
    label.setColour (juce::Label::textColourId, colour);
    label.setMinimumHorizontalScale (1.0f); // wrap, never shrink the text to fit
}

void ModalCard::stylePath (juce::Label& label, const juce::Colour& colour)
{
    label.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    label.setColour (juce::Label::textColourId, colour);
    label.setMinimumHorizontalScale (0.75f); // a long path shrinks rather than truncating
}

void ModalCard::setHeading (const juce::String& heading, const juce::String& subheading)
{
    headingLabel.setText (heading, juce::dontSendNotification);
    subheadingLabel.setText (subheading, juce::dontSendNotification);
    subheadingLabel.setVisible (subheading.isNotEmpty());
}

int ModalCard::headingBlockHeight() const
{
    // Two lines of room for the subheading: these are sentences, not labels,
    // and one of them ("Every take gets its own folder...") does not fit in one.
    return 30 + (subheadingLabel.isVisible() ? 8 + 34 : 0);
}

int ModalCard::getRequiredHeight() const
{
    return kCardPadding * 2 + headingBlockHeight() + 16 + getContentHeight();
}

juce::Rectangle<int> ModalCard::getCardBounds() const
{
    const int width = juce::jmin (kCardWidth, juce::jmax (280, getWidth() - 32));
    const int height = juce::jmin (getRequiredHeight(), juce::jmax (160, getHeight() - 32));

    return juce::Rectangle<int> (width, height).withCentre (getLocalBounds().getCentre());
}

void ModalCard::resized()
{
    auto card = getCardBounds().reduced (kCardPadding);

    headingLabel.setBounds (card.removeFromTop (30));

    if (subheadingLabel.isVisible())
    {
        card.removeFromTop (8);
        subheadingLabel.setBounds (card.removeFromTop (34));
    }

    card.removeFromTop (16);
    layOutContent (card);
}

void ModalCard::paint (juce::Graphics& g)
{
    // Dim rather than hide: the meters stay visible behind the card, so it
    // still reads as this application asking, not as a system dialog arriving.
    g.fillAll (AppLookAndFeel::background.withAlpha (0.86f));

    const auto card = getCardBounds().toFloat();

    g.setColour (AppLookAndFeel::surface);
    g.fillRoundedRectangle (card, 12.0f);

    g.setColour (AppLookAndFeel::outline);
    g.drawRoundedRectangle (card.reduced (0.5f), 12.0f, 1.0f);
}

} // namespace mma
