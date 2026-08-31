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

    // A rounded card with air around it, not a panel painted to the edges.
    // Filling the whole component made every neighbouring strip share a hard
    // seam, so a row of channels read as one striped slab rather than as
    // separate channels.
    auto card = bounds.reduced (1.0f);
    g.setColour (kPanel);
    g.fillRoundedRectangle (card, 8.0f);

    // §14.6: this mic is the one being heard right now. A ring rather than a
    // fill so it reads at a glance without fighting the level display.
    if (highlighted)
    {
        g.setColour (kBone); // §9.2 palette, same bone white as the peak bar
        g.drawRoundedRectangle (card.reduced (1.0f), 7.0f, 2.0f);
    }

    // Taller and narrower than before: the meter is now the vertical element of
    // a channel strip rather than a square badge with a caption underneath.
    auto skullBounds = bounds.withTrimmedBottom (bounds.getHeight() * 0.34f).reduced (3.0f);
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

        // 1.5px of full-strength bone made the outline, not the level inside it,
        // the loudest thing on the card. It still defines the shape at 52px.
        g.setColour (kBone.withAlpha (0.72f));
        juce::PathStrokeType outlineStroke (1.2f);
        g.strokePath (silhouette, outlineStroke);

        // Peak-hold bar: 2.5px bone-white line at the peak position.
        const float peakNorm = juce::jlimit (0.0f, 1.0f,
                                             (currentPeakDb - Metering::kMinDb) / (Metering::kMaxDb - Metering::kMinDb));
        const float peakY = clipRegion.getBottom() - clipRegion.getHeight() * peakNorm;
        // Inset to the skull's own width. Run edge to edge it read as a rule
        // drawn across the card -- a divider rather than this channel's peak.
        const float peakInset = clipRegion.getWidth() * 0.14f;
        g.setColour (kBone);
        g.fillRect (clipRegion.getX() + peakInset, peakY - 1.0f,
                    clipRegion.getWidth() - peakInset * 2.0f, 2.0f);

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

    // Channel-strip footer: the level and the name stacked tight under the
    // meter, the way a mixing desk labels a fader. This used to be three
    // centred lines spread across the bottom 30% of a wide box, which read as
    // a caption rather than as a channel.
    auto textArea = bounds.withTrimmedTop (bounds.getHeight() * 0.66f);

    // Numeric level first and closest to the meter, because it is the thing
    // that moves and the thing being read while levels are set.
    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain));
    g.setColour (noSignal ? kTertiaryText : kBone);
    const juce::String levelText = noSignal ? juce::String ("--.-")
                                            : juce::String (currentLevelDb, 1);
    g.drawText (levelText, textArea.removeFromTop (13.0f).toNearestInt(),
                juce::Justification::centred);

    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 9.0f, juce::Font::plain));
    g.setColour (kTertiaryText);
    g.drawText (noSignal ? juce::String ("pk --.-") : ("pk " + juce::String (currentPeakDb, 1)),
                textArea.removeFromTop (11.0f).toNearestInt(),
                juce::Justification::centred);

    textArea.removeFromTop (3.0f);

    // The name last and largest of the labels: on a desk the strip is
    // identified at its foot, and it is what someone points at when they say
    // "turn that one down".
    g.setFont (juce::Font (12.0f, juce::Font::bold));
    g.setColour (kBone);
    g.drawText (micName, textArea.removeFromTop (15.0f).toNearestInt(),
                juce::Justification::centred, true);

    g.setFont (juce::Font (9.0f, juce::Font::plain));
    g.setColour (kSecondaryText);
    g.drawText (deviceName, textArea.removeFromTop (11.0f).toNearestInt(),
                juce::Justification::centred, true);
}

void SkullMeterComponent::resized() {}

} // namespace mma
