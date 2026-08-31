#include "SavedTakePanel.h"
#include "AppLookAndFeel.h"

namespace mma {

namespace {
    constexpr int kRowPitch = 20;
    // Past this the list scrolls rather than the card growing off the screen.
    constexpr int kMaxVisibleRows = 9;
}

SavedTakePanel::SavedTakePanel()
{
    setHeading ("Saved.", {});

    stylePath (folderValue, AppLookAndFeel::bone);
    addAndMakeVisible (folderValue);

    styleBody (totalLabel, AppLookAndFeel::secondary);
    addAndMakeVisible (totalLabel);

    listViewport.setViewedComponent (&listContainer, false);
    listViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (listViewport);

    styleBody (mirrorValue, AppLookAndFeel::secondary);
    addChildComponent (mirrorValue);

    // §10.6: what happened, then what to do, in one sentence and no code.
    styleBody (emptyWarning, AppLookAndFeel::warning);
    emptyWarning.setText ("These files are empty. Check that the microphones aren't muted "
                          "at their own switches, then record again.", juce::dontSendNotification);
    addChildComponent (emptyWarning);

    // §6.5 "alert loudly". Danger red rather than the warning amber the
    // empty-files line uses: this is not something to look at later, it is the
    // take having been ended by the hardware.
    styleBody (problemLabel, AppLookAndFeel::danger);
    addChildComponent (problemLabel);

    // The offer §6.2 asks for, and the reason this panel exists rather than a
    // status line: a path someone has to retype is a path they will not visit.
    openButton.setColour (juce::TextButton::buttonColourId, AppLookAndFeel::accent);
    openButton.setColour (juce::TextButton::textColourOffId, AppLookAndFeel::background);
    openButton.onClick = [this] { if (onOpenFolder) onOpenFolder(); };
    addAndMakeVisible (openButton);

    doneButton.onClick = [this] { if (onDone) onDone(); };
    addAndMakeVisible (doneButton);
}

SavedTakePanel::~SavedTakePanel() = default;

void SavedTakePanel::setTake (const juce::String& folder,
                              const juce::String& mirrorFolder,
                              const std::vector<FileRow>& files)
{
    folderValue.setText (folder, juce::dontSendNotification);

    rows.clear();
    listContainer.removeAllChildren();

    int64_t total = 0;
    int64_t audioBytes = 0;
    int audioFiles = 0;

    for (const auto& file : files)
    {
        total += file.sizeBytes;

        // session.json is a few hundred bytes whatever happened, so it must not
        // be what makes an otherwise empty take look like it recorded something.
        if (! file.name.endsWithIgnoreCase (".json"))
        {
            audioBytes += file.sizeBytes;
            ++audioFiles;
        }

        Row row;
        row.name = std::make_unique<juce::Label>();
        stylePath (*row.name, AppLookAndFeel::bone);
        row.name->setText (file.name, juce::dontSendNotification);

        row.size = std::make_unique<juce::Label>();
        stylePath (*row.size, AppLookAndFeel::secondary);
        row.size->setText (juce::File::descriptionOfSizeInBytes (file.sizeBytes),
                           juce::dontSendNotification);
        row.size->setJustificationType (juce::Justification::centredRight);

        listContainer.addAndMakeVisible (*row.name);
        listContainer.addAndMakeVisible (*row.size);
        rows.push_back (std::move (row));
    }

    totalLabel.setText (juce::String (files.size()) + " files, "
                            + juce::File::descriptionOfSizeInBytes (total),
                        juce::dontSendNotification);

    // A WAV header alone is 44 bytes plus the BWF chunk, so anything under a
    // kilobyte per file holds no audio worth the name.
    everythingWasEmpty = audioFiles > 0 && audioBytes < audioFiles * 1024;
    emptyWarning.setVisible (everythingWasEmpty);

    mirrorValue.setVisible (mirrorFolder.isNotEmpty());
    mirrorValue.setText ("A second copy is in " + mirrorFolder, juce::dontSendNotification);

    listHeight = juce::jmin ((int) rows.size(), kMaxVisibleRows) * kRowPitch;

    resized();
}

void SavedTakePanel::setProblem (const juce::String& text)
{
    problemLabel.setText (text, juce::dontSendNotification);
    problemLabel.setVisible (text.isNotEmpty());

    // §9.3: the heading carries it too, not just the colour. "Saved." over a
    // take the drive cut short would be the app agreeing with a user who thinks
    // nothing went wrong.
    setHeading (text.isNotEmpty() ? "The recording was stopped." : "Saved.", {});

    resized();
}

bool SavedTakePanel::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey || key == juce::KeyPress::returnKey)
    {
        if (onDone)
            onDone();

        return true;
    }

    return false;
}

int SavedTakePanel::getContentHeight() const
{
    int height = kRowHeight + 2 + kRowHeight   // the folder, then the file count
               + 12 + listHeight;

    if (problemLabel.isVisible())
        height += 52 + 12;

    if (mirrorValue.isVisible())
        height += 10 + kRowHeight;

    if (emptyWarning.isVisible())
        height += 10 + 34;

    return height + 18 + kButtonHeight;
}

void SavedTakePanel::layOutContent (juce::Rectangle<int> area)
{
    // Above the folder and the files: what happened comes before what survived.
    if (problemLabel.isVisible())
    {
        problemLabel.setBounds (area.removeFromTop (52));
        area.removeFromTop (12);
    }

    folderValue.setBounds (area.removeFromTop (kRowHeight));
    area.removeFromTop (2);
    totalLabel.setBounds (area.removeFromTop (kRowHeight));

    area.removeFromTop (12);
    listViewport.setBounds (area.removeFromTop (listHeight));

    const int contentWidth = listViewport.getWidth()
                           - (listViewport.isVerticalScrollBarShown() ? listViewport.getScrollBarThickness() : 0);
    listContainer.setSize (juce::jmax (1, contentWidth), (int) rows.size() * kRowPitch);

    int y = 0;
    for (auto& row : rows)
    {
        juce::Rectangle<int> line (0, y, listContainer.getWidth(), kRowPitch);
        row.size->setBounds (line.removeFromRight (juce::jmin (96, line.getWidth() / 3)));
        row.name->setBounds (line);
        y += kRowPitch;
    }

    if (mirrorValue.isVisible())
    {
        area.removeFromTop (10);
        mirrorValue.setBounds (area.removeFromTop (kRowHeight));
    }

    if (emptyWarning.isVisible())
    {
        area.removeFromTop (10);
        emptyWarning.setBounds (area.removeFromTop (34));
    }

    area.removeFromTop (18);
    auto buttons = area.removeFromTop (kButtonHeight);
    doneButton.setBounds (buttons.removeFromLeft (90));
    openButton.setBounds (buttons.removeFromRight (juce::jmin (170, buttons.getWidth())));
}

} // namespace mma
