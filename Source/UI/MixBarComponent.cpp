#include "MixBarComponent.h"

namespace mma {

const juce::Colour MixBarComponent::kPanel         { 0xFF1E1816 };
const juce::Colour MixBarComponent::kEmptyInterior { 0xFF2A2320 };
const juce::Colour MixBarComponent::kFillLow       { 0xFF7A9E7E };
const juce::Colour MixBarComponent::kFillMid       { 0xFFD9A441 };
const juce::Colour MixBarComponent::kFillHigh      { 0xFFC3352B };
const juce::Colour MixBarComponent::kBone          { 0xFFEDE4D3 };
const juce::Colour MixBarComponent::kSecondaryText { 0xFF8C8177 };

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
    g.setColour (kEmptyInterior);
    g.fillRoundedRectangle (bounds, 4.0f);

    const float norm = juce::jlimit (0.0f, 1.0f, (currentLevelDb - Metering::kMinDb) / (Metering::kMaxDb - Metering::kMinDb));
    auto fillBounds = bounds.withWidth (bounds.getWidth() * norm);

    juce::Colour fillColour = currentLevelDb < -18.0f ? kFillLow : (currentLevelDb < -3.0f ? kFillMid : kFillHigh);
    g.setColour (fillColour);
    g.fillRoundedRectangle (fillBounds, 4.0f);

    g.setColour (kBone);
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    g.setColour (kSecondaryText);
    juce::String label = "MIX  " + juce::String (currentLevelDb, 1) + " dBFS" + (currentClip ? "  CLIP" : "");
    g.drawText (label, bounds.toNearestInt(), juce::Justification::centredLeft);
}

} // namespace mma
