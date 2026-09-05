#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Core/HelpTopics.h"
#include <functional>
#include <vector>

namespace mma {

/// The Help screen: the answers to "why is it silent?" in the app, one door
/// from the main screen, laid out as headings over plain paragraphs.
///
/// The words live in Core/HelpTopics so a test can hold them to account; this
/// class only draws them. Everything is painted rather than built from labels
/// because a juce::Label wraps only to the height it is given, and the height
/// a paragraph needs is exactly what this screen has to measure to size
/// itself -- so it measures once with a TextLayout and paints with the same.
class HelpPanel : public juce::Component
{
public:
    HelpPanel();
    ~HelpPanel() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    /// The height the whole screen needs at its current width. Text wraps, so
    /// this changes with the width; a container sets the width first and asks
    /// after.
    int getRequiredHeight() const;

    std::function<void()> onCloseClicked;
    std::function<void()> onOpenSettingsClicked;
    std::function<void()> onExportDiagnosticsClicked;

private:
    juce::TextButton closeButton { "< Done" };
    juce::TextButton settingsButton { "Open Settings" };
    juce::TextButton diagnosticsButton { "Export diagnostics" };

    std::vector<HelpTopic> topics;

    /// Width the text is measured and painted at: the panel's, less margins,
    /// with a floor so an unsized panel still measures something sensible.
    int textWidth() const;

    /// Where each topic's body starts and how tall it is, at the current
    /// width. Rebuilt by resized() and read by paint(), so the two agree.
    struct Block { int headingY = 0, ruleY = 0, bodyY = 0, bodyHeight = 0; };
    std::vector<Block> blocks;
    int introHeight = 0;
    int contentBottom = 0;

    static juce::Font headingFont();
    static juce::Font bodyFont();
    static juce::Font introFont();
    static juce::AttributedString bodyText (const juce::String& text, const juce::Font& font, juce::Colour colour);
    static int measure (const juce::AttributedString& text, int width);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HelpPanel)
};

} // namespace mma
