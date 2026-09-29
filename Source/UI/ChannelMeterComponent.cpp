#include "ChannelMeterComponent.h"
#include "AppLookAndFeel.h"

namespace mma {

// These were a second copy of the §9.2 hex values. They are now built from
// the one set in AppLookAndFeel, so a recolour happens once rather than four
// times and cannot leave this file behind.
const juce::Colour ChannelMeterComponent::kPanel             { palette::surface };
const juce::Colour ChannelMeterComponent::kBone              { palette::bone };
const juce::Colour ChannelMeterComponent::kEmptyInterior     { palette::surfaceHigh };
const juce::Colour ChannelMeterComponent::kFillLow           { palette::meterLow };
const juce::Colour ChannelMeterComponent::kFillMid           { palette::meterMid };
const juce::Colour ChannelMeterComponent::kFillHigh          { palette::meterHigh };
const juce::Colour ChannelMeterComponent::kClipEyes          { palette::clipEyes };
const juce::Colour ChannelMeterComponent::kDimmedOutline     { palette::dimmedOutline };
const juce::Colour ChannelMeterComponent::kTertiaryText      { palette::tertiary };

ChannelMeterComponent::ChannelMeterComponent()
{
    setWantsKeyboardFocus (true);
    // Tab still reaches a strip, but a mouse click leaves the keyboard where
    // it was. A clicked strip that kept the focus took the next Space -- the
    // room's mute -- to clear a clip or reopen its rename dialog.
    setMouseClickGrabsKeyboardFocus (false);
    setAccessible (true);
    updateAccessibilityText();
    startTimerHz (60); // §8.2: UI polls at 60Hz, independent of the audio callback
}

ChannelMeterComponent::~ChannelMeterComponent()
{
    stopTimer();
}

void ChannelMeterComponent::timerCallback()
{
    if (metering == nullptr)
        return;

    const bool wasClipped = currentClip;
    const int previousClipCount = currentClipCount;
    currentLevelDb = metering->tick (1.0 / 60.0);
    currentPeakDb = metering->getPeakHoldDb();
    currentClip = metering->isClipped();
    currentClipCount = metering->getClipCount();

    // Keep a screen reader's value useful without publishing a 60 Hz stream
    // of tiny level changes. Whole decibels, clip transitions and clip-count
    // changes are the same information a sighted user can actually read.
    const int accessibleLevel = juce::roundToInt (currentLevelDb);
    if (accessibleLevel != lastAccessibleLevelDb
        || currentClip != wasClipped || currentClipCount != previousClipCount)
    {
        lastAccessibleLevelDb = accessibleLevel;
        updateAccessibilityText();
    }

    repaint();
}

void ChannelMeterComponent::mouseUp (const juce::MouseEvent&)
{
    performPrimaryAction();
}

void ChannelMeterComponent::performPrimaryAction()
{
    // Tap to acknowledge and clear the clip latch (§9.1). Clearing a
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

bool ChannelMeterComponent::keyPressed (const juce::KeyPress& key)
{
    // This component exposes AccessibilityRole::button. Both Return and Space
    // activate a focused button; letting Space bubble instead triggered the
    // main window's global monitor-mute shortcut and made keyboard activation
    // do something unrelated and potentially alarming.
    if (key.getKeyCode() != juce::KeyPress::returnKey
        && key != juce::KeyPress::spaceKey)
        return false;

    performPrimaryAction();
    return true;
}

std::unique_ptr<juce::AccessibilityHandler> ChannelMeterComponent::createAccessibilityHandler()
{
    juce::AccessibilityActions actions;
    actions.addAction (juce::AccessibilityActionType::press,
                       [this] { performPrimaryAction(); });

    // A strip behaves like a named button: activate once to clear a clip, or
    // to open its rename dialog when it is healthy. Supplying the press action
    // is what lets VoiceOver and Narrator do the same thing as Return/click.
    return std::make_unique<juce::AccessibilityHandler> (
        *this, juce::AccessibilityRole::button, std::move (actions));
}

void ChannelMeterComponent::updateAccessibilityText()
{
    const auto readableName = micName.isNotEmpty() ? micName : juce::String ("Unnamed microphone");
    setTitle (readableName + " meter");

    juce::String description;
    if (deviceName.isNotEmpty())
        description << deviceName << ". ";

    if (noSignal)
        description << "No signal. ";
    else
        description << juce::String (currentLevelDb, 0) << " decibels. ";

    if (currentClip)
        description << "Clipping, " << juce::jmax (1, currentClipCount)
                    << (currentClipCount == 1 ? " clip. " : " clips. ")
                    << "Press Return or Space to clear the clip warning. ";
    else
        description << "Press Return or Space to rename this microphone. ";

    setDescription (description.trim());
}

void ChannelMeterComponent::setHighlighted (bool shouldHighlight)
{
    if (highlighted == shouldHighlight)
        return;

    highlighted = shouldHighlight;
    repaint();
}

juce::Colour ChannelMeterComponent::fillColourForLevel (float levelDb) const
{
    if (levelDb < -18.0f) return kFillLow;
    if (levelDb < -3.0f) return kFillMid;
    return kFillHigh;
}

void ChannelMeterComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    paintStrip (g, bounds);

    // Keyboard navigation must be as visible as pointer hover. This ring is
    // deliberately outside the meter's colour vocabulary so it cannot be
    // mistaken for the highlighted/tapped microphone state.
    if (hasKeyboardFocus (true))
    {
        g.setColour (AppLookAndFeel::accent);
        g.drawRoundedRectangle (bounds.reduced (2.0f), 8.0f, 2.5f);
    }
}

juce::Path ChannelMeterComponent::teardrop (juce::Point<float> tip, float height)
{
    // A point at the top, round at the bottom: the app icon's tear.
    const float r = height * 0.34f;
    const juce::Point<float> centre (tip.x, tip.y + height - r);

    juce::Path p;
    p.startNewSubPath (tip);
    p.quadraticTo (tip.x + r * 1.05f, centre.y - r * 0.35f, centre.x + r, centre.y);
    p.addCentredArc (centre.x, centre.y, r, r, 0.0f,
                     juce::MathConstants<float>::halfPi,
                     juce::MathConstants<float>::pi * 1.5f);
    p.quadraticTo (tip.x - r * 1.05f, centre.y - r * 0.35f, tip.x, tip.y);
    p.closeSubPath();
    return p;
}

void ChannelMeterComponent::paintFace (juce::Graphics& g, juce::Rectangle<float> badge, float norm)
{
    // The badge is the app icon in miniature: two eyes and a frown inside the
    // ring. It is decoration on top of the level, never the level itself -- the
    // fill, the track and the number still say everything (§9.3).
    const float w = badge.getWidth();
    const float cx = badge.getCentreX();
    const float eyeY = badge.getY() + w * 0.40f;
    const float eyeDx = w * 0.17f;
    const float eyeR = juce::jmax (1.2f, w * 0.055f);

    // Features sit on either the dark well or the bright fill, so they flip
    // tone once the level has risen past them.
    const bool eyesUnderFill = norm > 0.58f;
    const bool mouthUnderFill = norm > 0.30f;
    const auto ink = [&] (bool underFill)
    {
        if (noSignal) return kDimmedOutline.withAlpha (0.8f);
        return underFill ? juce::Colour (palette::background).withAlpha (0.85f)
                         : kBone.withAlpha (0.85f);
    };

    if (noSignal)
    {
        // Nobody here: eyes closed, a flat mouth. Not sad yet, just asleep.
        g.setColour (ink (false));
        for (float dx : { -eyeDx, eyeDx })
            g.drawLine (cx + dx - eyeR, eyeY, cx + dx + eyeR, eyeY, 1.2f);
        g.drawLine (cx - w * 0.12f, badge.getY() + w * 0.68f,
                    cx + w * 0.12f, badge.getY() + w * 0.68f, 1.2f);
        return;
    }

    g.setColour (ink (eyesUnderFill));
    for (float dx : { -eyeDx, eyeDx })
        g.fillEllipse (cx + dx - eyeR, eyeY - eyeR, eyeR * 2.0f, eyeR * 2.0f);

    // The frown.
    juce::Path mouth;
    const float mouthY = badge.getY() + w * 0.72f;
    mouth.startNewSubPath (cx - w * 0.15f, mouthY);
    mouth.quadraticTo (cx, mouthY - w * 0.13f, cx + w * 0.15f, mouthY);
    g.setColour (ink (mouthUnderFill));
    g.strokePath (mouth, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // Tears: one once somebody is really going for it, both on a clip -- a
    // full sob. Cyan, like the one tear on the icon.
    const auto face = getFace();
    if (face != Face::OneTear && face != Face::Sob)
        return;

    const float tearH = w * 0.26f;
    g.setColour (juce::Colour (palette::accent));
    g.fillPath (teardrop ({ cx - eyeDx, eyeY + eyeR + 0.5f }, tearH));
    if (face == Face::Sob)
        g.fillPath (teardrop ({ cx + eyeDx, eyeY + eyeR + 0.5f }, tearH));
}

juce::Rectangle<float> ChannelMeterComponent::badgeFor (juce::Rectangle<float> bounds)
{
    auto inner = bounds.reduced (1.0f).reduced (10.0f, 6.0f);
    const float badgeSize = juce::jmin (inner.getHeight(), 26.0f);
    return inner.removeFromLeft (badgeSize).withSizeKeepingCentre (badgeSize, badgeSize);
}

juce::Rectangle<float> ChannelMeterComponent::getBadgeBounds() const
{
    return badgeFor (getLocalBounds().toFloat());
}

ChannelMeterComponent::Face ChannelMeterComponent::getFace() const noexcept
{
    if (noSignal)                           return Face::Asleep;
    if (currentClip)                        return Face::Sob;
    if (currentLevelDb >= kTearThresholdDb) return Face::OneTear;
    return Face::Frown;
}

void ChannelMeterComponent::paintStrip (juce::Graphics& g, juce::Rectangle<float> bounds)
{
    auto card = bounds.reduced (1.0f);
    g.setColour (kPanel);
    g.fillRoundedRectangle (card, 10.0f);

    // §14.6: a ring around the channel currently being heard.
    if (highlighted)
    {
        g.setColour (kBone);
        g.drawRoundedRectangle (card.reduced (1.0f), 9.0f, 2.0f);
    }

    auto inner = card.reduced (10.0f, 6.0f);

    // The badge: a ring that fills from the bottom with the level, with the
    // app icon's crying face drawn inside it.
    const float badgeSize = juce::jmin (inner.getHeight(), 26.0f);
    inner.removeFromLeft (badgeSize);
    const auto badge = badgeFor (bounds);

    if (noSignal)
    {
        g.setColour (kDimmedOutline.withAlpha (0.55f));
        g.drawEllipse (badge.reduced (1.5f), 1.5f);
        paintFace (g, badge, 0.0f);
    }
    else
    {
        const float norm = juce::jlimit (0.0f, 1.0f,
                                         (currentLevelDb - Metering::kMinDb)
                                             / (Metering::kMaxDb - Metering::kMinDb));

        g.setColour (kEmptyInterior);
        g.fillEllipse (badge.reduced (1.5f));

        // Filled from the bottom, like a level.
        {
            juce::Graphics::ScopedSaveState save (g);
            auto fill = badge.withTop (badge.getBottom() - badge.getHeight() * norm);
            g.reduceClipRegion (fill.toNearestInt());
            g.setColour (fillColourForLevel (currentLevelDb));
            g.fillEllipse (badge.reduced (1.5f));
        }

        g.setColour (currentClip ? kClipEyes : kBone.withAlpha (0.72f));
        g.drawEllipse (badge.reduced (1.5f), currentClip ? 2.0f : 1.2f);

        paintFace (g, badge, norm);
    }

    inner.removeFromLeft (10.0f);

    // The number is right-aligned and reserved first, so the track between the
    // name and it does not change length as the level moves.
    auto valueArea = inner.removeFromRight (58.0f);
    inner.removeFromRight (8.0f);

    // §9.3: clip says itself in words and in a count, not in a hue. The
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

        // Peak hold, as a bone tick.
        const float peakNorm = juce::jlimit (0.0f, 1.0f,
                                             (currentPeakDb - Metering::kMinDb)
                                                 / (Metering::kMaxDb - Metering::kMinDb));
        const float peakX = track.getX() + track.getWidth() * peakNorm;
        g.setColour (kBone);
        g.fillRect (juce::Rectangle<float> (peakX - 1.0f, track.getY() - 3.0f, 2.0f,
                                            track.getHeight() + 6.0f));
    }
}

void ChannelMeterComponent::resized() {}

} // namespace mma
