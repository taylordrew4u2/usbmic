#include "MixBarComponent.h"
#include "AppLookAndFeel.h"

namespace mma {

// Built from the one palette, for the reason given in ChannelMeterComponent.
const juce::Colour MixBarComponent::kPanel           { palette::surface };
const juce::Colour MixBarComponent::kEmptyInterior   { palette::surfaceHigh };
const juce::Colour MixBarComponent::kFillLow         { palette::meterLow };
const juce::Colour MixBarComponent::kFillMid         { palette::meterMid };
const juce::Colour MixBarComponent::kFillHigh        { palette::meterHigh };
const juce::Colour MixBarComponent::kBone            { palette::bone };
const juce::Colour MixBarComponent::kSecondaryText   { palette::secondary };
const juce::Colour MixBarComponent::kOutline         { palette::outline };

MixBarComponent::MixBarComponent()
{
    // Every channel strip has a spoken name and level; the mix had neither,
    // so VoiceOver announced an unnamed "group" in the row of meters and the
    // one level that is the room's sum could not be read at all.
    setTitle ("Mix meter");
    updateAccessibilityText();
    startTimerHz (60);
}

MixBarComponent::~MixBarComponent() { stopTimer(); }

void MixBarComponent::timerCallback()
{
    if (metering == nullptr)
        return;
    const bool wasClipped = currentClip;
    currentLevelDb = metering->tick (1.0 / 60.0);
    currentPeakDb = metering->getPeakHoldDb();
    currentClip = metering->isClipped();

    // Whole decibels, like the strips: what a sighted user can read, without
    // a 60 Hz stream of value changes for a screen reader.
    const int accessibleLevel = juce::roundToInt (currentLevelDb);
    if (accessibleLevel != lastAccessibleLevelDb || currentClip != wasClipped)
    {
        lastAccessibleLevelDb = accessibleLevel;
        updateAccessibilityText();
    }

    if (! repaintPaused)
        repaint();
}

void MixBarComponent::updateAccessibilityText()
{
    setDescription (juce::String (currentLevelDb, 0) + " decibels"
                    + (currentClip ? ". Clipping." : "."));
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
    // peaked and fell back left nothing on it, while every channel meter beside
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
