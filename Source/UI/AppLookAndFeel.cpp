#include "AppLookAndFeel.h"

namespace mma {

const juce::Colour AppLookAndFeel::background  { 0xFF16110F };
const juce::Colour AppLookAndFeel::surface     { 0xFF1E1816 };
const juce::Colour AppLookAndFeel::surfaceHigh { 0xFF2A2320 };
const juce::Colour AppLookAndFeel::bone        { 0xFFEDE4D3 };
const juce::Colour AppLookAndFeel::secondary   { 0xFF8C8177 };
const juce::Colour AppLookAndFeel::tertiary    { 0xFF5E554D };
const juce::Colour AppLookAndFeel::accent      { 0xFFD9A441 };
const juce::Colour AppLookAndFeel::danger      { 0xFFC3352B };
const juce::Colour AppLookAndFeel::outline     { 0xFF3A312C };

AppLookAndFeel::AppLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::DocumentWindow::backgroundColourId,  background);

    setColour (juce::Label::textColourId,            bone);
    setColour (juce::Label::backgroundColourId,      juce::Colours::transparentBlack);

    setColour (juce::TextButton::buttonColourId,     surface);
    setColour (juce::TextButton::buttonOnColourId,   accent);
    setColour (juce::TextButton::textColourOffId,    bone);
    setColour (juce::TextButton::textColourOnId,     background);

    setColour (juce::ComboBox::backgroundColourId,   surface);
    setColour (juce::ComboBox::textColourId,         bone);
    setColour (juce::ComboBox::outlineColourId,      outline);
    setColour (juce::ComboBox::arrowColourId,        secondary);
    setColour (juce::ComboBox::buttonColourId,       surface);

    setColour (juce::PopupMenu::backgroundColourId,           surface);
    setColour (juce::PopupMenu::textColourId,                 bone);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, surfaceHigh);
    setColour (juce::PopupMenu::highlightedTextColourId,      accent);

    setColour (juce::TextEditor::backgroundColourId,      surface);
    setColour (juce::TextEditor::textColourId,            bone);
    setColour (juce::TextEditor::outlineColourId,         outline);
    setColour (juce::TextEditor::focusedOutlineColourId,  accent);
    setColour (juce::TextEditor::highlightColourId,       accent.withAlpha (0.28f));
    setColour (juce::TextEditor::highlightedTextColourId, bone);
    setColour (juce::CaretComponent::caretColourId,       accent);

    setColour (juce::Slider::backgroundColourId,      surfaceHigh);
    setColour (juce::Slider::trackColourId,           accent);
    setColour (juce::Slider::thumbColourId,           bone);
    setColour (juce::Slider::textBoxTextColourId,     bone);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId,  juce::Colours::transparentBlack);

    setColour (juce::ScrollBar::thumbColourId,        outline.brighter (0.35f));
    setColour (juce::ScrollBar::trackColourId,        juce::Colours::transparentBlack);
    setColour (juce::ScrollBar::backgroundColourId,   juce::Colours::transparentBlack);

    setColour (juce::ToggleButton::textColourId,         bone);
    setColour (juce::ToggleButton::tickColourId,         accent);
    setColour (juce::ToggleButton::tickDisabledColourId, tertiary);
}

void AppLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                           const juce::Colour& backgroundColour,
                                           bool shouldDrawButtonAsHighlighted,
                                           bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const float radius = 6.0f;

    auto fill = backgroundColour;

    if (shouldDrawButtonAsDown)
        fill = fill.brighter (0.18f);
    else if (shouldDrawButtonAsHighlighted)
        fill = fill.brighter (0.08f);

    g.setColour (fill);
    g.fillRoundedRectangle (bounds, radius);

    // One hairline instead of a bevel. It is what separates the control from the
    // background at this contrast without drawing attention to itself.
    g.setColour (outline);
    g.drawRoundedRectangle (bounds, radius, 1.0f);
}

void AppLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                       float sliderPos, float, float,
                                       juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
                                          0.0f, 0.0f, style, slider);
        return;
    }

    const float trackHeight = 4.0f;
    const float centreY = y + height * 0.5f;

    juce::Rectangle<float> track (static_cast<float> (x), centreY - trackHeight * 0.5f,
                                  static_cast<float> (width), trackHeight);

    g.setColour (surfaceHigh);
    g.fillRoundedRectangle (track, trackHeight * 0.5f);

    // Filled to the thumb, so the level is readable without a number beside it.
    g.setColour (accent);
    g.fillRoundedRectangle (track.withWidth (juce::jmax (trackHeight, sliderPos - x)),
                            trackHeight * 0.5f);

    const float thumbRadius = 7.0f;
    g.setColour (bone);
    g.fillEllipse (sliderPos - thumbRadius, centreY - thumbRadius,
                   thumbRadius * 2.0f, thumbRadius * 2.0f);
}

void AppLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&,
                                    int x, int y, int width, int height,
                                    bool isScrollbarVertical, int thumbStartPosition,
                                    int thumbSize, bool isMouseOver, bool isMouseDown)
{
    juce::Rectangle<int> thumb;

    if (isScrollbarVertical)
        thumb = { x + width / 3, thumbStartPosition, juce::jmax (2, width / 3), thumbSize };
    else
        thumb = { thumbStartPosition, y + height / 3, thumbSize, juce::jmax (2, height / 3) };

    // Present but recessive, and only brightening under the pointer. JUCE's
    // default draws this in its accent blue at full strength, which on a dark
    // warm background was the loudest thing on screen.
    auto colour = outline.brighter (isMouseDown ? 0.6f : (isMouseOver ? 0.4f : 0.15f));

    g.setColour (colour);
    g.fillRoundedRectangle (thumb.toFloat(), thumb.getWidth() * 0.5f);
}

void AppLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                       bool shouldDrawButtonAsHighlighted, bool)
{
    const float boxSize = 16.0f;
    auto bounds = button.getLocalBounds().toFloat();

    juce::Rectangle<float> box (bounds.getX(), bounds.getCentreY() - boxSize * 0.5f,
                                boxSize, boxSize);

    g.setColour (button.getToggleState() ? accent : surface);
    g.fillRoundedRectangle (box, 3.0f);

    g.setColour (button.getToggleState() ? accent
                                         : (shouldDrawButtonAsHighlighted ? secondary : outline));
    g.drawRoundedRectangle (box, 3.0f, 1.0f);

    if (button.getToggleState())
    {
        // Drawn rather than a glyph, so it stays crisp at any size.
        juce::Path tick;
        tick.startNewSubPath (box.getX() + boxSize * 0.24f, box.getCentreY());
        tick.lineTo (box.getX() + boxSize * 0.44f, box.getY() + boxSize * 0.70f);
        tick.lineTo (box.getX() + boxSize * 0.78f, box.getY() + boxSize * 0.30f);

        g.setColour (background);
        g.strokePath (tick, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    }

    g.setColour (button.isEnabled() ? bone : tertiary);
    g.setFont (14.0f);
    g.drawText (button.getButtonText(),
                bounds.withTrimmedLeft (boxSize + 10.0f),
                juce::Justification::centredLeft, true);
}

} // namespace mma
