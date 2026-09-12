#include "AppLookAndFeel.h"

namespace mma {

const juce::Colour AppLookAndFeel::background     { palette::background };
const juce::Colour AppLookAndFeel::surface        { palette::surface };
const juce::Colour AppLookAndFeel::surfaceHigh    { palette::surfaceHigh };
const juce::Colour AppLookAndFeel::bone           { palette::bone };
const juce::Colour AppLookAndFeel::secondary      { palette::secondary };
const juce::Colour AppLookAndFeel::tertiary       { palette::tertiary };
const juce::Colour AppLookAndFeel::accent         { palette::accent };
const juce::Colour AppLookAndFeel::danger         { palette::danger };
const juce::Colour AppLookAndFeel::warning        { palette::warning };
const juce::Colour AppLookAndFeel::outline        { palette::outline };
const juce::Colour AppLookAndFeel::meterLow       { palette::meterLow };
const juce::Colour AppLookAndFeel::meterMid       { palette::meterMid };
const juce::Colour AppLookAndFeel::meterHigh      { palette::meterHigh };
const juce::Colour AppLookAndFeel::clipEyes       { palette::clipEyes };
const juce::Colour AppLookAndFeel::dimmedOutline  { palette::dimmedOutline };

AppLookAndFeel::AppLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::DocumentWindow::backgroundColourId,  background);

    setColour (juce::Label::textColourId,            bone);
    setColour (juce::Label::backgroundColourId,      juce::Colours::transparentBlack);

    // One step lighter than any panel, so a button is visible before it is
    // hovered. On `surface` it vanished into the Settings screen.
    setColour (juce::TextButton::buttonColourId,     surfaceHigh);
    setColour (juce::TextButton::buttonOnColourId,   accent);
    setColour (juce::TextButton::textColourOffId,    bone);
    setColour (juce::TextButton::textColourOnId,     background);

    setColour (juce::ComboBox::backgroundColourId,   surfaceHigh);
    setColour (juce::ComboBox::textColourId,         bone);
    setColour (juce::ComboBox::outlineColourId,      outline);
    setColour (juce::ComboBox::arrowColourId,        secondary);
    setColour (juce::ComboBox::buttonColourId,       surfaceHigh);

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

    // Hover and press are unmistakable rather than subtle: a button that
    // brightens by a few percent under the pointer gives no sign that it has
    // noticed, and people click twice.
    if (shouldDrawButtonAsDown)
        fill = fill.brighter (0.35f);
    else if (shouldDrawButtonAsHighlighted)
        fill = fill.brighter (0.18f);

    if (! button.isEnabled())
        fill = fill.withAlpha (0.5f);

    g.setColour (fill);
    g.fillRoundedRectangle (bounds, radius);

    // A visible edge. A bright fill (the record button) gets a darker edge of
    // its own tone; a dark fill gets the shared control outline.
    const bool keyboardFocus = button.hasKeyboardFocus (true);
    g.setColour (keyboardFocus
                     ? accent
                     : (backgroundColour.getPerceivedBrightness() > 0.5f
                            ? backgroundColour.darker (0.3f)
                            : (shouldDrawButtonAsHighlighted ? secondary : controlOutline())));
    g.drawRoundedRectangle (bounds.reduced (keyboardFocus ? 1.0f : 0.0f), radius,
                            keyboardFocus ? 2.5f : 1.0f);
}

juce::Colour AppLookAndFeel::controlOutline()
{
    return outline.brighter (0.9f);
}

juce::Font AppLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    // Readable at every size the app uses; the default scaled with the button
    // and came out at 11px on the smaller ones.
    return juce::Font (juce::jlimit (14.0f, 17.0f, buttonHeight * 0.42f));
}

void AppLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                                   int, int, int, int, juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);
    const float radius = 6.0f;
    const bool hovered = box.isMouseOver (true);
    const bool keyboardFocus = box.hasKeyboardFocus (true);

    auto fill = box.findColour (juce::ComboBox::backgroundColourId);
    if (isButtonDown)  fill = fill.brighter (0.35f);
    else if (hovered)  fill = fill.brighter (0.18f);

    g.setColour (fill);
    g.fillRoundedRectangle (bounds, radius);

    g.setColour (keyboardFocus ? accent : (hovered ? secondary : controlOutline()));
    g.drawRoundedRectangle (bounds.reduced (keyboardFocus ? 1.0f : 0.0f), radius,
                            keyboardFocus ? 2.5f : 1.0f);

    // A chevron large enough to read as "this opens", rather than JUCE's
    // small triangle in the outline tone.
    const float cx = width - height * 0.5f;
    const float cy = height * 0.5f;
    const float half = juce::jmin (6.0f, height * 0.18f);

    juce::Path chevron;
    chevron.startNewSubPath (cx - half, cy - half * 0.5f);
    chevron.lineTo (cx, cy + half * 0.5f);
    chevron.lineTo (cx + half, cy - half * 0.5f);

    g.setColour (box.isEnabled() ? bone : tertiary);
    g.strokePath (chevron, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
}

juce::Font AppLookAndFeel::getComboBoxFont (juce::ComboBox&) { return juce::Font (14.0f); }
juce::Font AppLookAndFeel::getPopupMenuFont()                 { return juce::Font (14.0f); }

void AppLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator,
                                                int standardMenuItemHeight, int& idealWidth,
                                                int& idealHeight)
{
    LookAndFeel_V4::getIdealPopupMenuItemSize (text, isSeparator, standardMenuItemHeight,
                                               idealWidth, idealHeight);

    // Menu rows tall enough to land on. The sample-rate and storage menus are
    // the pickers people use most, and their rows were 22px apart.
    if (! isSeparator)
        idealHeight = juce::jmax (idealHeight, 32);
}

juce::MouseCursor AppLookAndFeel::getMouseCursorFor (juce::Component& component)
{
    if (component.isEnabled()
        && (dynamic_cast<juce::Button*> (&component) != nullptr
            || dynamic_cast<juce::ComboBox*> (&component) != nullptr))
        return juce::MouseCursor::PointingHandCursor;

    return LookAndFeel_V4::getMouseCursorFor (component);
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

    if (slider.hasKeyboardFocus (true))
    {
        g.setColour (accent);
        g.drawRoundedRectangle (slider.getLocalBounds().toFloat().reduced (1.5f), 6.0f, 2.5f);
    }
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
    const float boxSize = 20.0f;
    auto bounds = button.getLocalBounds().toFloat();

    // The whole row is the target, and the whole row says so under the
    // pointer. The box alone was a 16px square that was easy to miss.
    if (shouldDrawButtonAsHighlighted && button.isEnabled())
    {
        g.setColour (surfaceHigh.withAlpha (0.6f));
        g.fillRoundedRectangle (bounds, 6.0f);
    }

    juce::Rectangle<float> box (bounds.getX() + 4.0f, bounds.getCentreY() - boxSize * 0.5f,
                                boxSize, boxSize);

    g.setColour (button.getToggleState() ? accent : surface);
    g.fillRoundedRectangle (box, 3.0f);

    g.setColour (button.getToggleState() ? accent
                                         : (shouldDrawButtonAsHighlighted ? bone : controlOutline()));
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

    if (button.hasKeyboardFocus (true))
    {
        g.setColour (accent);
        g.drawRoundedRectangle (bounds.reduced (1.5f), 6.0f, 2.5f);
    }

    g.setColour (button.isEnabled() ? bone : tertiary);
    g.setFont (14.0f);
    g.drawText (button.getButtonText(),
                bounds.withTrimmedLeft (boxSize + 14.0f),
                juce::Justification::centredLeft, true);
}

} // namespace mma
