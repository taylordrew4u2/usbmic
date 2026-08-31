#include "MixBarComponent.h"

namespace mma {

const juce::Colour MixBarComponent::kPanel         { 0xFF1E1816 };
const juce::Colour MixBarComponent::kEmptyInterior { 0xFF2A2320 };
const juce::Colour MixBarComponent::kFillLow       { 0xFF7A9E7E };
const juce::Colour MixBarComponent::kFillMid       { 0xFFD9A441 };
const juce::Colour MixBarComponent::kFillHigh      { 0xFFC3352B };
const juce::Colour MixBarComponent::kBone          { 0xFFEDE4D3 };
const juce::Colour MixBarComponent::kSecondaryText { 0xFF8C8177 };
const juce::Colour MixBarComponent::kOutline       { 0xFF3A312C };

MixBarComponent::MixBarComponent() { startTimerHz (60); }
MixBarComponent::~MixBarComponent() { stopTimer(); }

void MixBarComponent::timerCallback()
{
    if (metering == nullptr)
        return;
    currentLevelDb = metering->tick (1.0 / 60.0);
    currentPeakDb = metering->getPeakHoldDb();
    currentClip = metering->isClipped();
    repaint();
}

void MixBarComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    constexpr float kRadius = 4.0f;

    g.setColour (kEmptyInterior);
    g.fillRoundedRectangle (bounds, kRadius);

    const float norm = juce::jlimit (0.0f, 1.0f, (currentLevelDb - Metering::kMinDb) / (Metering::kMaxDb - Metering::kMinDb));

    juce::Colour fillColour = currentLevelDb < -18.0f ? kFillLow : (currentLevelDb < -3.0f ? kFillMid : kFillHigh);

    // Clip the full-width rounded bar to the level, rather than rounding the
    // fill itself. Rounding a narrow fill gave a lozenge with two curved ends
    // floating at the left of the track; this keeps the fill flush with the
    // track's own left corners and square where it stops.
    if (norm > 0.0f)
    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (bounds.withWidth (bounds.getWidth() * norm).getSmallestIntegerContainer());
        g.setColour (fillColour);
        g.fillRoundedRectangle (bounds, kRadius);
    }

    // A hairline in the outline tone. At full-strength bone the frame was the
    // brightest thing in the row, competing with the fill it contains.
    g.setColour (kOutline);
    g.drawRoundedRectangle (bounds, kRadius, 1.0f);

    // Name on the left, number on the right, both inside the track's padding.
    // Left-justified into the very corner, the label sat on the fill and was
    // unreadable the moment the mix got loud.
    auto textArea = bounds.reduced (8.0f, 0.0f).toNearestInt();

    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::bold));
    g.setColour (kBone.withAlpha (0.85f));
    g.drawText ("MIX", textArea, juce::Justification::centredLeft);

    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain));
    g.setColour (currentClip ? kFillHigh : kSecondaryText);
    g.drawText (juce::String (currentLevelDb, 1) + " dBFS" + (currentClip ? "   CLIP" : ""),
                textArea, juce::Justification::centredRight);
}

} // namespace mma
