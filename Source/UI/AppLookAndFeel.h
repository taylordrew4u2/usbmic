#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace mma {

/// The §9.2 palette applied to JUCE's own controls.
///
/// Without this the app painted its own components in the warm palette and got
/// stock JUCE for everything else -- so scrollbars, slider thumbs, text-field
/// outlines and the record button all arrived in JUCE's default blue, against a
/// near-black warm background. That mismatch, not the layout, is what made the
/// window look unfinished.
///
/// Setting the colour ids in one place also means a control added later inherits
/// the palette instead of reintroducing the same problem.
class AppLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // Taken from SkullMeterComponent, which defines §9.2.
    static const juce::Colour background;   // warm near-black
    static const juce::Colour surface;      // raised panels and fields
    static const juce::Colour surfaceHigh;  // hover, pressed
    static const juce::Colour bone;         // primary text
    static const juce::Colour secondary;    // supporting text
    static const juce::Colour tertiary;     // hints, disabled
    static const juce::Colour accent;       // amber: the one colour that acts
    static const juce::Colour danger;       // recording, clipping
    static const juce::Colour outline;      // hairlines

    AppLookAndFeel();

    // Flat, rounded, no gradient and no bevel -- the shape carries the affordance.
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

    // A slim track with a round thumb, rather than JUCE's default blue bar.
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    // Quiet until touched: a thin bar that does not compete with the meters.
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                        bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted,
                           bool shouldDrawButtonAsDown) override;
};

} // namespace mma
