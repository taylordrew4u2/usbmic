#include "SkullMeterComponent.h"
#include "AppLookAndFeel.h"

namespace mma {

// These were a second copy of the §9.2 hex values. They are now built from
// the one set in AppLookAndFeel, so a recolour happens once rather than four
// times and cannot leave this file behind.
const juce::Colour SkullMeterComponent::kBackground        { palette::background };
const juce::Colour SkullMeterComponent::kPanel             { palette::surface };
const juce::Colour SkullMeterComponent::kBone              { palette::bone };
const juce::Colour SkullMeterComponent::kEmptyInterior     { palette::surfaceHigh };
const juce::Colour SkullMeterComponent::kFillLow           { palette::meterLow };
const juce::Colour SkullMeterComponent::kFillMid           { palette::meterMid };
const juce::Colour SkullMeterComponent::kFillHigh          { palette::meterHigh };
const juce::Colour SkullMeterComponent::kClipEyes          { palette::clipEyes };
const juce::Colour SkullMeterComponent::kDimmedOutline     { palette::dimmedOutline };
const juce::Colour SkullMeterComponent::kSecondaryText     { palette::secondary };
const juce::Colour SkullMeterComponent::kTertiaryText      { palette::tertiary };

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
    currentClipCount = metering->getClipCount();
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

    if (orientation == Orientation::Strip)
        paintStrip (g, bounds);
    else
        paintTall (g, bounds);
}

void SkullMeterComponent::paintTall (juce::Graphics& g, juce::Rectangle<float> bounds)
{
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
    paintSkull (g, bounds.withTrimmedBottom (bounds.getHeight() * 0.34f).reduced (3.0f), true);

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

    // §9.3: "every coloured state also changes shape, text, or number", and
    // "clip indication cannot depend on hue". The eyes change colour, and the
    // glow they also carried is switched off for prefers-reduced-motion -- which
    // left hue alone carrying it for exactly the readers §9.3 protects. So the
    // clip says itself in words and in a count, on the line that is already
    // here: no row appears or disappears, so nothing below it moves when a
    // channel clips.
    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 9.0f, juce::Font::plain));
    g.setColour (currentClip ? kClipEyes : kTertiaryText);

    const juce::String peakLine = currentClip
        ? ("CLIP " + juce::String (juce::jmax (1, currentClipCount)))
        : (noSignal ? juce::String ("pk --.-") : ("pk " + juce::String (currentPeakDb, 1)));

    g.drawText (peakLine, textArea.removeFromTop (11.0f).toNearestInt(),
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


void SkullMeterComponent::paintSkull (juce::Graphics& g, juce::Rectangle<float> skullBounds,
                                      bool withEyes)
{
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

        // Eye sockets: clip indicator. The colour and the glow are the glance;
        // the "CLIP n" line under the meter is what carries the meaning when
        // the glow is off for reduced motion and hue is all that is left
        // (§9.3). This comment used to claim that text existed. It did not.
        if (! withEyes)
            return;

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

}

void SkullMeterComponent::paintStrip (juce::Graphics& g, juce::Rectangle<float> bounds)
{
    auto card = bounds.reduced (1.0f);
    g.setColour (kPanel);
    g.fillRoundedRectangle (card, 10.0f);

    // §14.6, same meaning as in the tall strip: a ring around the channel
    // currently being heard.
    if (highlighted)
    {
        g.setColour (kBone);
        g.drawRoundedRectangle (card.reduced (1.0f), 9.0f, 2.0f);
    }

    auto inner = card.reduced (10.0f, 6.0f);

    // The badge: a ring that fills from the bottom with the level, echoing the
    // app's own mark. Not the skull silhouette -- squeezed into a 26px square
    // it loses its jaw and its sockets and reads as a flowerpot, which is the
    // exact failure that took the skull off the icon. The tall meter keeps the
    // skull; at that size it is still a skull.
    const float badgeSize = juce::jmin (inner.getHeight(), 26.0f);
    auto badge = inner.removeFromLeft (badgeSize).withSizeKeepingCentre (badgeSize, badgeSize);

    if (noSignal)
    {
        g.setColour (kDimmedOutline.withAlpha (0.55f));
        g.drawEllipse (badge.reduced (1.5f), 1.5f);
    }
    else
    {
        const float norm = juce::jlimit (0.0f, 1.0f,
                                         (currentLevelDb - Metering::kMinDb)
                                             / (Metering::kMaxDb - Metering::kMinDb));

        g.setColour (kEmptyInterior);
        g.fillEllipse (badge.reduced (1.5f));

        // Filled from the bottom, exactly as the skull fills from the jaw, so
        // the two orientations mean the same thing.
        {
            juce::Graphics::ScopedSaveState save (g);
            auto fill = badge.withTop (badge.getBottom() - badge.getHeight() * norm);
            g.reduceClipRegion (fill.toNearestInt());
            g.setColour (fillColourForLevel (currentLevelDb));
            g.fillEllipse (badge.reduced (1.5f));
        }

        g.setColour (currentClip ? kClipEyes : kBone.withAlpha (0.72f));
        g.drawEllipse (badge.reduced (1.5f), currentClip ? 2.0f : 1.2f);
    }

    inner.removeFromLeft (10.0f);

    // The number is right-aligned and reserved first, so the track between the
    // name and it does not change length as the level moves.
    auto valueArea = inner.removeFromRight (58.0f);
    inner.removeFromRight (8.0f);

    // §9.3: clip says itself in words and in a count, not in a hue. This is the
    // strip's version of the tall layout's "CLIP n" line -- same rule, and the
    // number it replaces is the one the reader is already looking at.
    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain));
    g.setColour (currentClip ? kClipEyes : (noSignal ? kTertiaryText : kBone));
    g.drawText (currentClip ? ("CLIP " + juce::String (juce::jmax (1, currentClipCount)))
                            : (noSignal ? juce::String ("--.-") : juce::String (currentLevelDb, 1)),
                valueArea.toNearestInt(), juce::Justification::centredRight);

    // A fixed budget, so every strip's track starts at the same x and the row
    // reads as a column of levels. Taken as a fraction of what was left after
    // the track was reserved, this came out at ~33px and every name on the
    // screen rendered as an ellipsis.
    const float nameWidth = juce::jlimit (60.0f, 120.0f, inner.getWidth() * 0.45f);
    auto nameArea = inner.removeFromLeft (nameWidth);
    g.setFont (juce::Font (12.0f, juce::Font::bold));
    g.setColour (kBone);
    g.drawText (micName, nameArea.toNearestInt(), juce::Justification::centredLeft, true);

    inner.removeFromLeft (8.0f);

    // The track. A hairline well with the level laid over it, so an idle
    // channel still shows where its level would appear rather than showing
    // nothing at all -- an empty row and a broken row must not look alike.
    auto track = inner.withSizeKeepingCentre (inner.getWidth(), 4.0f);
    g.setColour (kEmptyInterior);
    g.fillRoundedRectangle (track, 2.0f);

    if (! noSignal)
    {
        const float norm = juce::jlimit (0.0f, 1.0f,
                                         (currentLevelDb - Metering::kMinDb)
                                             / (Metering::kMaxDb - Metering::kMinDb));

        if (norm > 0.0f)
        {
            g.setColour (fillColourForLevel (currentLevelDb));
            g.fillRoundedRectangle (track.withWidth (track.getWidth() * norm), 2.0f);
        }

        // Peak hold, as a bone tick rather than a bar: the same information the
        // tall meter draws across the skull, in the space this one has.
        const float peakNorm = juce::jlimit (0.0f, 1.0f,
                                             (currentPeakDb - Metering::kMinDb)
                                                 / (Metering::kMaxDb - Metering::kMinDb));
        const float peakX = track.getX() + track.getWidth() * peakNorm;
        g.setColour (kBone);
        g.fillRect (juce::Rectangle<float> (peakX - 1.0f, track.getY() - 3.0f, 2.0f,
                                            track.getHeight() + 6.0f));
    }
}

void SkullMeterComponent::resized() {}

} // namespace mma
