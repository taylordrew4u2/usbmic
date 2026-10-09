#pragma once
#include "ModalCard.h"
#include "../Core/SessionRecovery.h"
#include <functional>
#include <memory>
#include <vector>

namespace mma {

/// §6.6: "on launch, scan the last-used card and the mirror directory for
/// sessions lacking a stop timestamp. Repair headers, present recovered
/// recordings before the main screen."
///
/// This is the presenting half. It comes up in front of the main screen at
/// launch and only then -- most launches follow a clean quit and it never
/// appears at all, which is why it says what happened rather than assuming the
/// user knows why a panel is in front of them.
class RecoveredTakesPanel : public ModalCard
{
public:
    RecoveredTakesPanel();
    ~RecoveredTakesPanel() override;

    /// Built by recoveredTakeRow(), which is also where the card's wording
    /// lives -- so it can be tested without a window.
    using TakeRow = RecoveredTakeRow;

    void setTakes (const std::vector<TakeRow>& takes);

    /// The take the Open button will reveal -- the newest, which is the one the
    /// crash actually interrupted.
    juce::String getFolderToOpen() const { return folderToOpen; }

    std::function<void()> onOpenFolder;
    std::function<void()> onDone;

    bool keyPressed (const juce::KeyPress& key) override;
    void prepareToShow() { doneButton.grabKeyboardFocus(); }

protected:
    int getContentHeight() const override;
    void layOutContent (juce::Rectangle<int> area) override;

private:
    struct Row
    {
        std::unique_ptr<juce::Label> name;
        std::unique_ptr<juce::Label> detail;
    };

    /// The name, and the detail under it on as many lines as it takes at
    /// `width`: the camera-movie and backup-copy notes do not fit on one
    /// beside the sound's, and a one-line label cut them off.
    static int rowHeight (const Row& row, int width);

    juce::Label explanation;
    std::vector<Row> rows;
    juce::String folderToOpen;

    juce::TextButton openButton { "Open the folder" };
    juce::TextButton doneButton { "Done" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecoveredTakesPanel)
};

} // namespace mma
