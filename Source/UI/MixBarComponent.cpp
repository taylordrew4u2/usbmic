#include "MixBarComponent.h"
#include "AppLookAndFeel.h"

namespace mma {

// Built from the one palette, for the reason given in SkullMeterComponent.
const juce::Colour MixBarComponent::kPanel           { palette::surface };
const juce::Colour MixBarComponent::kEmptyInterior   { palette::surfaceHigh };
const juce::Colour MixBarComponent::kFillLow         { palette::meterLow };
const juce::Colour MixBarComponent::kFillMid         { palette::meterMid };
const juce::Colour MixBarComponent::kFillHigh        { palette::meterHigh };
const juce::Colour MixBarComponent::kBone            { palette::bone };
const juce::Colour MixBarComponent::kSecondaryText   { palette::secondary };
const juce::Colour MixBarComponent::kOutline         { palette::outline };

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

    // Peak hold. currentPeakDb was sampled every frame and never drawn, so the
    // mix bus was the one meter with no trace of a transient: a passage that
    // peaked and fell back left nothing on it, while every skull meter beside
    // it showed its "pk" figure. A transient on the MIX is exactly the thing
    // worth seeing, because it is what the limiter is catching.
    //
    // Drawn before the outline so the frame stays on top of it.
    if (currentPeakDb > Metering::kMinDb)
    {
        const float peakNorm = juce::jlimit (0.0f, 1.0f,
            (currentPeakDb - Metering::kMinDb) / (Metering::kMaxDb - Metering::kMinDb));

        // Inset by the line width so a full-scale peak lands on the track
        // rather than half outside it.
        const float x = bounds.getX()
                      + juce::jlimit (1.0f, bounds.getWidth() - 1.0f,
                                      bounds.getWidth() * peakNorm);

        g.setColour (currentPeakDb >= -3.0f ? kFillHigh : kBone.withAlpha (0.7f));
        g.fillRect (x - 1.0f, bounds.getY() + 1.0f, 2.0f, bounds.getHeight() - 2.0f);
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
