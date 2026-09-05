#include "HelpPanel.h"
#include "AppLookAndFeel.h"

namespace mma {

namespace {
constexpr int kMargin       = 12;
constexpr int kCloseRow     = 30;
constexpr int kAfterClose   = 14;
constexpr int kIntroGap     = 18;
constexpr int kHeadingH     = 16;
constexpr int kHeadingGap   = 3;
constexpr int kRuleGap      = 9;
constexpr int kAfterTopic   = 20;
constexpr int kButtonRow    = 30;
constexpr int kMaxTextWidth = 640;  // a paragraph wider than this is hard to read
} // namespace

HelpPanel::HelpPanel()
    : topics (HelpTopics::all())
{
    closeButton.onClick = [this] { if (onCloseClicked) onCloseClicked(); };
    addAndMakeVisible (closeButton);

    // The two things the last topic tells the reader to go and do, as buttons
    // under it, so "Settings > Export diagnostics" is one click rather than a
    // trip back through the main screen.
    settingsButton.onClick = [this] { if (onOpenSettingsClicked) onOpenSettingsClicked(); };
    addAndMakeVisible (settingsButton);

    diagnosticsButton.onClick = [this] { if (onExportDiagnosticsClicked) onExportDiagnosticsClicked(); };
    addAndMakeVisible (diagnosticsButton);
}

HelpPanel::~HelpPanel() = default;

juce::Font HelpPanel::headingFont() { return juce::Font (10.0f, juce::Font::bold); }
juce::Font HelpPanel::bodyFont()    { return juce::Font (13.0f); }
juce::Font HelpPanel::introFont()   { return juce::Font (13.0f, juce::Font::italic); }

juce::AttributedString HelpPanel::bodyText (const juce::String& text, const juce::Font& font, juce::Colour colour)
{
    juce::AttributedString s;
    s.setJustification (juce::Justification::topLeft);
    s.setLineSpacing (3.0f);
    s.append (text, font, colour);
    return s;
}

int HelpPanel::measure (const juce::AttributedString& text, int width)
{
    juce::TextLayout layout;
    layout.createLayout (text, static_cast<float> (width));
    return static_cast<int> (std::ceil (layout.getHeight())) + 2;
}

int HelpPanel::textWidth() const
{
    // Measured against whatever width the panel has, with a floor for the
    // moment before a container has sized it: measuring at width 1 would
    // report one word per line and a screen a mile tall.
    const int available = juce::jmax (320, getWidth() - kMargin * 2);
    return juce::jmin (available, kMaxTextWidth);
}

int HelpPanel::getRequiredHeight() const
{
    const int width = textWidth();
    int y = kMargin + kCloseRow + kAfterClose;

    y += measure (bodyText (HelpTopics::introduction(), introFont(), AppLookAndFeel::secondary), width);
    y += kIntroGap;

    for (const auto& t : topics)
    {
        y += kHeadingH + kHeadingGap + 1 + kRuleGap;
        y += measure (bodyText (t.body, bodyFont(), AppLookAndFeel::bone), width);
        y += kAfterTopic;
    }

    y += kButtonRow + kMargin;
    return y;
}

void HelpPanel::resized()
{
    const int width = textWidth();
    const int x = kMargin;
    int y = kMargin;

    closeButton.setBounds (x, y, 110, kCloseRow);
    y += kCloseRow + kAfterClose;

    introHeight = measure (bodyText (HelpTopics::introduction(), introFont(), AppLookAndFeel::secondary), width);
    y += introHeight + kIntroGap;

    blocks.clear();
    for (const auto& t : topics)
    {
        Block b;
        b.headingY = y;
        y += kHeadingH + kHeadingGap;
        b.ruleY = y;
        y += 1 + kRuleGap;
        b.bodyY = y;
        b.bodyHeight = measure (bodyText (t.body, bodyFont(), AppLookAndFeel::bone), width);
        y += b.bodyHeight + kAfterTopic;
        blocks.push_back (b);
    }

    // The buttons sit under the last topic, which is the one that names them.
    settingsButton.setBounds (x, y, 130, kButtonRow);
    diagnosticsButton.setBounds (x + 130 + 10, y, 170, kButtonRow);
    contentBottom = y + kButtonRow;
}

void HelpPanel::paint (juce::Graphics& g)
{
    g.fillAll (AppLookAndFeel::surface);

    const int width = textWidth();
    const int x = kMargin;

    auto draw = [&] (const juce::AttributedString& text, int y, int height)
    {
        juce::TextLayout layout;
        layout.createLayout (text, static_cast<float> (width));
        layout.draw (g, juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y),
                                                static_cast<float> (width), static_cast<float> (height)));
    };

    draw (bodyText (HelpTopics::introduction(), introFont(), AppLookAndFeel::secondary),
          kMargin + kCloseRow + kAfterClose, introHeight);

    for (size_t i = 0; i < topics.size() && i < blocks.size(); ++i)
    {
        const auto& t = topics[i];
        const auto& b = blocks[i];

        // Headings in the same small capitals as the Settings screen's, so the
        // two doors off the main screen read as rooms in the same house.
        g.setColour (AppLookAndFeel::tertiary);
        g.setFont (headingFont());
        g.drawText (juce::String (t.heading).toUpperCase(), x, b.headingY, width, kHeadingH,
                    juce::Justification::centredLeft);

        g.setColour (AppLookAndFeel::outline);
        g.fillRect (x, b.ruleY, getWidth() - kMargin * 2, 1);

        draw (bodyText (t.body, bodyFont(), AppLookAndFeel::bone), b.bodyY, b.bodyHeight);
    }
}

} // namespace mma
