#include "SkullMeterComponent.h"

namespace mma {

const juce::Colour SkullMeterComponent::kBackground      { 0xFF16110F };
const juce::Colour SkullMeterComponent::kPanel           { 0xFF1E1816 };
const juce::Colour SkullMeterComponent::kBone            { 0xFFEDE4D3 };
const juce::Colour SkullMeterComponent::kEmptyInterior   { 0xFF2A2320 };
const juce::Colour SkullMeterComponent::kFillLow         { 0xFF7A9E7E };
const juce::Colour SkullMeterComponent::kFillMid         { 0xFFD9A441 };
const juce::Colour SkullMeterComponent::kFillHigh        { 0xFFC3352B };
const juce::Colour SkullMeterComponent::kClipEyes        { 0xFFF2C14A };
const juce::Colour SkullMeterComponent::kDimmedOutline   { 0xFF6E645B };
const juce::Colour SkullMeterComponent::kSecondaryText   { 0xFF8C8177 };
const juce::Colour SkullMeterComponent::kTertiaryText    { 0xFF5E554D };

SkullMeterComponent::SkullMeterComponent()
{
    startTimerHz (60); // §8.2: UI polls at 60Hz, independent of the audio callback
}

SkullMeterComponent::~SkullMeterComponent()
{
    stopTimer();
}

void SkullMeterComponent::timerCallback()
{
    if (metering == nullptr)
        return;

    currentLevelDb = metering->tick (1.0 / 60.0);
    currentPeakDb = metering->getPeakHoldDb();
    currentClip = metering->isClipped();
    repaint();
}

void SkullMeterComponent::mouseUp (const juce::MouseEvent&)
{
    // Tap the clip eyes to acknowledge and clear the latch (§9.1). Clearing a
    // clip is the click's first meaning; renaming takes the click only when
    // there is nothing to clear.
    if (metering != nullptr && currentClip)
    {
        metering->acknowledgeClip();
        return;
    }

    if (onNameClicked)
        onNameClicked();
}

void SkullMeterComponent::setHighlighted (bool shouldHighlight)
{
    if (highlighted == shouldHighlight)
        return;

    highlighted = shouldHighlight;
    repaint();
}

juce::Colour SkullMeterComponent::fillColourForLevel (float levelDb) const
{
    if (levelDb < -18.0f) return kFillLow;
    if (levelDb < -3.0f) return kFillMid;
    return kFillHigh;
}

juce::Path SkullMeterComponent::buildSkullSilhouette (juce::Rectangle<float> bounds) const
{
    // A simplified but recognisable skull silhouette: rounded cranium over a
    // tapered jaw, sized to stay legible down to 48px per §9.3. Real artwork
    // would replace this procedural path with designed vector art; this
    // keeps the meter's fill/clip/peak-hold behavior fully functional.
    juce::Path p;
    auto b = bounds;
    const float w = b.getWidth();
    const float h = b.getHeight();

    // Cranium: an ellipse for the top ~60% of the height.
    juce::Rectangle<float> cranium (b.getX() + w * 0.1f, b.getY(), w * 0.8f, h * 0.62f);
    p.addEllipse (cranium);

    // Jaw: a narrower rounded trapezoid for the bottom ~45%, overlapping the cranium.
    juce::Path jaw;
    jaw.startNewSubPath (b.getX() + w * 0.22f, b.getY() + h * 0.5f);
    jaw.lineTo (b.getX() + w * 0.78f, b.getY() + h * 0.5f);
    jaw.lineTo (b.getX() + w * 0.66f, b.getY() + h * 0.95f);
    jaw.quadraticTo (b.getCentreX(), b.getY() + h * 1.05f, b.getX() + w * 0.34f, b.getY() + h * 0.95f);
    jaw.closeSubPath();

    p.addPath (jaw);
    return p;
}

void SkullMeterComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.fillAll (kPanel);

    // §14.6: this mic is the one being heard right now. A ring rather than a
    // fill so it reads at a glance without fighting the level display.
    if (highlighted)
    {
        g.setColour (kBone); // §9.2 palette, same bone white as the peak bar
        g.drawRoundedRectangle (bounds.reduced (1.5f), 6.0f, 3.0f);
    }

    auto skullBounds = bounds.withTrimmedBottom (bounds.getHeight() * 0.3f).reduced (4.0f);
    juce::Path silhouette = buildSkullSilhouette (skullBounds);

    if (noSignal)
    {
        // §9.1: dashed hollow outline at 42% opacity when there's no signal.
        g.setColour (kDimmedOutline.withAlpha (0.42f));
        juce::PathStrokeType stroke (1.5f, juce::PathStrokeType::curved);
        float dashLengths[] = { 4.0f, 3.0f };
        juce::Path dashed;
        stroke.createDashedStroke (dashed, silhouette, dashLengths, 2);
        g.fillPath (dashed);
    }
    else
    {
        g.setColour (kEmptyInterior);
        g.fillPath (silhouette);

        // Fill from the jaw upward: map currentLevelDb from [-60,0] to [0,1].
        const float norm = juce::jlimit (0.0f, 1.0f,
                                         (currentLevelDb - Metering::kMinDb) / (Metering::kMaxDb - Metering::kMinDb));
        auto clipRegion = skullBounds;
        const float fillHeight = clipRegion.getHeight() * norm;
        auto fillRect = clipRegion.withTop (clipRegion.getBottom() - fillHeight);

        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (fillRect.toNearestInt());
            g.setColour (fillColourForLevel (currentLevelDb));
            g.fillPath (silhouette);
        }

        g.setColour (kBone);
        juce::PathStrokeType outlineStroke (1.5f);
        g.strokePath (silhouette, outlineStroke);

        // Peak-hold bar: 2.5px bone-white line at the peak position.
        const float peakNorm = juce::jlimit (0.0f, 1.0f,
                                             (currentPeakDb - Metering::kMinDb) / (Metering::kMaxDb - Metering::kMinDb));
        const float peakY = clipRegion.getBottom() - clipRegion.getHeight() * peakNorm;
        g.setColour (kBone);
        g.fillRect (clipRegion.getX(), peakY - 1.25f, clipRegion.getWidth(), 2.5f);

        // Eye sockets: clip indicator. Color changes AND an amber glow, but
        // the clip count text also changes -- color never carries meaning alone (§9.3).
        const float eyeY = skullBounds.getY() + skullBounds.getHeight() * 0.32f;
        const float eyeRadius = skullBounds.getWidth() * 0.09f;
        const float leftEyeX = skullBounds.getX() + skullBounds.getWidth() * 0.34f;
        const float rightEyeX = skullBounds.getX() + skullBounds.getWidth() * 0.66f;

        juce::Colour eyeColour = currentClip ? kClipEyes : kEmptyInterior.darker (0.3f);
        for (float ex : { leftEyeX, rightEyeX })
        {
            if (currentClip && ! reducedMotion)
            {
                g.setColour (kClipEyes.withAlpha (0.35f));
                g.fillEllipse (ex - eyeRadius * 1.6f, eyeY - eyeRadius * 1.6f, eyeRadius * 3.2f, eyeRadius * 3.2f);
            }
            g.setColour (eyeColour);
            g.fillEllipse (ex - eyeRadius, eyeY - eyeRadius, eyeRadius * 2.0f, eyeRadius * 2.0f);
        }
    }

    // Text row below the skull: name, device, numeric dBFS, peak -- monospace
    // so digits don't jitter (§9.3).
    auto textArea = bounds.withTrimmedTop (bounds.getHeight() * 0.7f);
    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));

    g.setColour (kBone);
    g.drawText (micName, textArea.removeFromTop (16.0f).toNearestInt(), juce::Justification::centred);

    g.setColour (kSecondaryText);
    g.drawText (deviceName, textArea.removeFromTop (14.0f).toNearestInt(), juce::Justification::centred);

    g.setColour (kTertiaryText);
    juce::String levelText = noSignal ? juce::String ("--.- dBFS")
                                       : juce::String (currentLevelDb, 1) + " dBFS  pk " + juce::String (currentPeakDb, 1);
    g.drawText (levelText, textArea.removeFromTop (14.0f).toNearestInt(), juce::Justification::centred);
}

void SkullMeterComponent::resized() {}

} // namespace mma
