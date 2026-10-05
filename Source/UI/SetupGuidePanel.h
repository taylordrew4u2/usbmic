#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Core/SetupGuide.h"
#include <functional>
#include <memory>
#include <vector>

namespace mma {

/// The first-run setup guide, one page at a time: plug in, name, headphones,
/// where takes go, cameras, a test take.
///
/// A drawer beside the main screen, like Settings and Help, rather than a card
/// over it. Two of its pages only work with the main screen in view -- tapping
/// a microphone lights its strip, and the test take is the record button
/// itself -- and a card that swallowed every click would stand between the
/// reader and the thing it is telling them to press. So nothing here is modal:
/// the meters stay live, Record still records, and Skip is on every page.
///
/// What each page says comes from Core/SetupGuide, so the words and the
/// branching can be tested without a window; this class only draws the page it
/// is given, the live lists under it, and reports which button was pressed.
class SetupGuidePanel : public juce::Component
{
public:
    SetupGuidePanel();
    ~SetupGuidePanel() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    /// The height the whole page needs at its current width. Text wraps, so a
    /// container sets the width first and asks after, as for HelpPanel.
    int getRequiredHeight() const;

    void setPage (const SetupGuidePage& page);
    const SetupGuidePage& getPage() const noexcept { return page; }

    /// One switched-on microphone, by its strip's name and the hardware's own.
    struct MicRow
    {
        juce::String name;
        juce::String product;
    };
    void setMicrophones (const std::vector<MicRow>& rows);

    /// The microphone heard alone just now (-1 for none), lit on the naming
    /// page the way its strip is lit on the main screen.
    void setHighlightedMic (int index);

    /// The same rows as Settings' "who hears the mix", and the same switch.
    struct HeadphoneRow
    {
        juce::String key, label;
        bool on = true;
    };
    void setHeadphones (const std::vector<HeadphoneRow>& rows);

    void setDestinationFolder (const juce::String& path);

    std::function<void()> onSkipClicked;
    std::function<void()> onBackClicked;
    std::function<void()> onNextClicked;
    std::function<void (int)> onNameMicClicked; // microphone index
    std::function<void (const juce::String& key, bool on)> onHeadphonesToggled;
    std::function<void()> onChangeFolderClicked;

private:
    SetupGuidePage page;
    std::vector<MicRow> mics;
    std::vector<HeadphoneRow> headphones;
    juce::String destination;
    int highlightedMic = -1;

    juce::TextButton skipButton { "Skip" };
    juce::TextButton backButton { "Back" };
    juce::TextButton nextButton { "Next" };
    juce::TextButton changeFolderButton { "Change..." };
    std::vector<std::unique_ptr<juce::TextButton>> nameButtons;
    std::vector<std::unique_ptr<juce::ToggleButton>> headphoneToggles;

    /// Where paint() draws what resized() measured, so the two agree.
    juce::Rectangle<int> counterArea, titleArea, bodyArea, statusArea, listArea, folderArea;

    /// Rows the current page lists under its text, and how tall each is.
    int listRowCount() const;
    int listRowHeight() const;

    /// Shows the children the current page uses and hides the rest.
    void updateChildVisibility();

    int textWidth() const;
    juce::AttributedString titleText() const;
    juce::AttributedString bodyText() const;
    juce::AttributedString statusText() const;
    static int measure (const juce::AttributedString& text, int width);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SetupGuidePanel)
};

} // namespace mma
