#include "RecoveredTakesPanel.h"
#include "AppLookAndFeel.h"

namespace mma {

namespace {
    constexpr int kRowPitch = 38;
    constexpr int kSecondDetailLine = 16;
    constexpr int kMaxDetailLines = 4;
    constexpr int kMaxListedTakes = 5;

    /// The lines JUCE will draw a take's detail on at `width`, worked out ahead
    /// of it so the row reserved is the row needed. JUCE's rule in
    /// GlyphArrangement: one line when it fits, otherwise the fewest lines
    /// that beat the text's width plus an 80px allowance for ragged breaks.
    /// The detail grew a clause at a time -- unrepaired files, empty ones, the
    /// camera movies, the backup copy -- and a count of clauses, rather than a
    /// measurement, cut the last of them off.
    int detailLines (const juce::Label& label, int width)
    {
        const auto textWidth = label.getFont().getStringWidthFloat (label.getText());
        const auto available = (float) juce::jmax (1, width - label.getBorderSize().getLeftAndRight());

        if (textWidth < available)
            return 1;

        return juce::jlimit (1, kMaxDetailLines, (int) ((textWidth + 80.0f) / available) + 1);
    }
}

RecoveredTakesPanel::RecoveredTakesPanel()
{
    setHeading ("Recovered.", {});

    // §10.6: what happened, then what it means, in plain language. A panel in
    // front of the main screen at launch has to say why it is there.
    styleBody (explanation, AppLookAndFeel::secondary);
    addAndMakeVisible (explanation);

    openButton.setColour (juce::TextButton::buttonColourId, AppLookAndFeel::accent);
    openButton.setColour (juce::TextButton::textColourOffId, AppLookAndFeel::background);
    openButton.onClick = [this] { if (onOpenFolder) onOpenFolder(); };
    addAndMakeVisible (openButton);

    doneButton.onClick = [this] { if (onDone) onDone(); };
    addAndMakeVisible (doneButton);
}

RecoveredTakesPanel::~RecoveredTakesPanel() = default;

void RecoveredTakesPanel::setTakes (const std::vector<TakeRow>& takes)
{
    // Destroying the labels detaches them: a juce::Component removes itself
    // from its parent as it goes. removeAllChildren() here would also take the
    // card's own heading with it.
    rows.clear();

    folderToOpen = takes.empty() ? juce::String() : juce::String::fromUTF8 (takes.front().fullPath.c_str());

    explanation.setText (juce::String::fromUTF8 (recoveredTakesExplanation (takes).c_str()),
                         juce::dontSendNotification);

    const int listed = juce::jmin ((int) takes.size(), kMaxListedTakes);

    for (int i = 0; i < listed; ++i)
    {
        const auto& take = takes[(size_t) i];

        Row row;
        row.name = std::make_unique<juce::Label>();
        stylePath (*row.name, AppLookAndFeel::bone);
        row.name->setText (juce::String::fromUTF8 (take.folderName.c_str()), juce::dontSendNotification);
        addAndMakeVisible (*row.name);

        row.detail = std::make_unique<juce::Label>();
        styleBody (*row.detail, AppLookAndFeel::secondary);
        row.detail->setText (juce::String::fromUTF8 (recoveredTakeDetail (take).c_str()),
                             juce::dontSendNotification);
        addAndMakeVisible (*row.detail);

        rows.push_back (std::move (row));
    }

    openButton.setButtonText (takes.size() > 1 ? "Open the newest" : "Open the folder");

    resized();
}

bool RecoveredTakesPanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey || key == juce::KeyPress::returnKey)
    {
        if (onDone)
            onDone();

        return true;
    }

    return false;
}

int RecoveredTakesPanel::rowHeight (const Row& row, int width)
{
    return kRowPitch + (detailLines (*row.detail, width) - 1) * kSecondDetailLine;
}

int RecoveredTakesPanel::getContentHeight() const
{
    // Measured at the width the card will have. Before the panel has been
    // given one, that is the card's full width, which is what it gets in any
    // window wide enough to show it whole.
    const int width = (getWidth() > 0 ? getCardWidth() : kCardWidth) - 2 * kCardPadding;
    int rowsHeight = 0;

    for (const auto& row : rows)
        rowsHeight += rowHeight (row, width);

    return 52 + 12 + rowsHeight + 18 + kButtonHeight;
}

void RecoveredTakesPanel::layOutContent (juce::Rectangle<int> area)
{
    explanation.setBounds (area.removeFromTop (52));
    area.removeFromTop (12);

    for (auto& row : rows)
    {
        auto line = area.removeFromTop (rowHeight (row, area.getWidth()));
        row.name->setBounds (line.removeFromTop (18));
        row.detail->setBounds (line);
    }

    area.removeFromTop (18);
    auto buttons = area.removeFromTop (kButtonHeight);
    doneButton.setBounds (buttons.removeFromLeft (90));
    openButton.setBounds (buttons.removeFromRight (juce::jmin (170, buttons.getWidth())));
}

} // namespace mma
