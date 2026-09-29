#pragma once
#include "AppLookAndFeel.h"
#include "ChannelMeterComponent.h"
#include <cstdlib>

namespace mma {

/// Reads a channel strip's badge back out of pixels.
///
/// ChannelMeterComponent::getFace() says what the face should be; this says
/// what was actually drawn, by rendering the component the way the screen does
/// and counting pixels of each palette colour inside its badge. The simulators
/// check the two agree, so a face that decides to cry but paints nothing -- or
/// a tear left over from a louder moment -- fails a test instead of shipping.
struct MeterFaceProbe
{
    int tearLeft = 0;    ///< accent pixels left of the badge's centre
    int tearRight = 0;   ///< accent pixels right of it
    int clipRing = 0;    ///< clipEyes pixels in the badge
    int fillLow = 0, fillMid = 0, fillHigh = 0;
    int highlightRing = 0; ///< bone pixels along the card's left edge
    int badgePixels = 0;

    int tears() const noexcept { return tearLeft + tearRight; }

    /// Enough accent pixels on one side to be a tear, rather than a stray
    /// anti-aliased edge where two other colours happen to blend.
    static constexpr int kTearMinPixels = 6;

    bool tearOnLeft() const noexcept  { return tearLeft >= kTearMinPixels; }
    bool tearOnRight() const noexcept { return tearRight >= kTearMinPixels; }

    static MeterFaceProbe of (ChannelMeterComponent& meter, float scale = 3.0f)
    {
        const auto image = meter.createComponentSnapshot (meter.getLocalBounds(), true, scale);
        return of (image, meter.getBadgeBounds() * scale, scale);
    }

    static MeterFaceProbe of (const juce::Image& image, juce::Rectangle<float> badge, float scale)
    {
        MeterFaceProbe p;
        const juce::Image::BitmapData px (image, juce::Image::BitmapData::readOnly);
        const auto area = badge.getSmallestIntegerContainer().getIntersection (
            { 0, 0, image.getWidth(), image.getHeight() });
        const float cx = badge.getCentreX();

        for (int y = area.getY(); y < area.getBottom(); ++y)
            for (int x = area.getX(); x < area.getRight(); ++x)
            {
                const auto c = px.getPixelColour (x, y);
                ++p.badgePixels;
                if (near (c, palette::accent))        (x < cx ? p.tearLeft : p.tearRight)++;
                else if (near (c, palette::clipEyes)) ++p.clipRing;
                else if (near (c, palette::meterLow)) ++p.fillLow;
                else if (near (c, palette::meterMid)) ++p.fillMid;
                else if (near (c, palette::meterHigh)) ++p.fillHigh;
            }

        // The highlight ring runs round the card's edge; its left side is the
        // one stretch no other bone-coloured element (the peak tick, the name)
        // ever reaches.
        const int edgeX = juce::roundToInt (2.5f * scale);
        for (int y = image.getHeight() / 4; y < image.getHeight() * 3 / 4; ++y)
            for (int x = juce::jmax (0, edgeX - 2); x <= edgeX + 2 && x < image.getWidth(); ++x)
                if (near (px.getPixelColour (x, y), palette::bone))
                    ++p.highlightRing;

        return p;
    }

    static bool near (juce::Colour c, juce::uint32 target, int tolerance = 34) noexcept
    {
        const juce::Colour t (target);
        return c.getAlpha() > 200
            && std::abs (c.getRed()   - t.getRed())   <= tolerance
            && std::abs (c.getGreen() - t.getGreen()) <= tolerance
            && std::abs (c.getBlue()  - t.getBlue())  <= tolerance;
    }
};

} // namespace mma
