#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace mma {

/// The §9.2 palette as plain ARGB values.
///
/// These are integers rather than juce::Colour objects on purpose. The meters,
/// the mix bar and the main screen each want their own named constants, and a
/// `juce::Colour` initialised from another translation unit's `juce::Colour` is
/// a static-initialisation-order bug waiting for a build that happens to order
/// the objects the other way -- it would read an unconstructed colour and paint
/// the app black. A constexpr integer is constant-initialised, so every file can
/// build its own Colour from the one set of values with no ordering to get wrong.
namespace palette {
    inline constexpr juce::uint32 background    = 0xFF0A0E13; // slate near-black
    inline constexpr juce::uint32 surface       = 0xFF121A23; // panels, cards, fields
    inline constexpr juce::uint32 surfaceHigh   = 0xFF1C2733; // hover, pressed, empty well
    inline constexpr juce::uint32 bone          = 0xFFE3EAF2; // primary text, skull outline
    inline constexpr juce::uint32 secondary     = 0xFF8496A8; // supporting text
    inline constexpr juce::uint32 tertiary      = 0xFF566372; // hints, disabled
    inline constexpr juce::uint32 accent        = 0xFF22D3EE; // cyan: the one colour that acts
    inline constexpr juce::uint32 danger        = 0xFFFF4D5E; // recording, clipping
    inline constexpr juce::uint32 warning       = 0xFFFBBF24; // something needs attention
    inline constexpr juce::uint32 outline       = 0xFF27333F; // hairlines

    // Green through yellow to red is the convention every level meter uses, so
    // these keep that order however cool the rest of the palette gets: the
    // palette is not worth relearning a meter for.
    inline constexpr juce::uint32 meterLow      = 0xFF2DD4A7;
    inline constexpr juce::uint32 meterMid      = 0xFFFACC15;
    inline constexpr juce::uint32 meterHigh     = 0xFFFF4D5E;
    inline constexpr juce::uint32 clipEyes      = 0xFFFDE047; // the skull's eyes, latched on clip
    inline constexpr juce::uint32 dimmedOutline = 0xFF4A5B6D; // dashed skull, no signal
} // namespace palette


/// The §9.2 palette applied to JUCE's own controls.
///
/// Without this the app painted its own components in the house palette and got
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
    // Named Colour objects over `palette`, for the many call sites that want a
    // colour rather than an integer.
    static const juce::Colour background;
    static const juce::Colour surface;
    static const juce::Colour surfaceHigh;
    static const juce::Colour bone;
    static const juce::Colour secondary;
    static const juce::Colour tertiary;
    static const juce::Colour accent;
    static const juce::Colour danger;
    static const juce::Colour warning;
    static const juce::Colour outline;
    static const juce::Colour meterLow;
    static const juce::Colour meterMid;
    static const juce::Colour meterHigh;
    static const juce::Colour clipEyes;
    static const juce::Colour dimmedOutline;

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
