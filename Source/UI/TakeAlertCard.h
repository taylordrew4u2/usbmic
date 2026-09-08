#pragma once
#include "ModalCard.h"
#include <functional>
#include <memory>
#include <vector>

namespace mma {

/// §0.1 / §6.5: the pop-up for something going wrong in the middle of a take.
/// A microphone unplugged, a camera switched off, sound dropped, the drive
/// falling behind or running out of room. Each is one line, newest at the
/// bottom, with when in the take it happened. The take carries on behind the
/// card either way; the two buttons are "carry on" and "stop now".
class TakeAlertCard : public ModalCard
{
public:
    TakeAlertCard();
    ~TakeAlertCard() override;

    /// Adds a line. `recovery` draws it in the calm tone (something came back).
    void addAlert (const juce::String& whenInTake, const juce::String& message, bool recovery);
    void clear();
    int getAlertCount() const { return static_cast<int> (rows.size()); }

    std::function<void()> onKeepRecording;
    std::function<void()> onStopRecording;

    bool keyPressed (const juce::KeyPress& key) override;
    void prepareToShow() { keepButton.grabKeyboardFocus(); }

protected:
    int getContentHeight() const override;
    void layOutContent (juce::Rectangle<int> area) override;

private:
    struct Row { std::unique_ptr<juce::Label> when, text; };
    std::vector<Row> rows;

    juce::TextButton keepButton { "Keep recording" };
    juce::TextButton stopButton { "Stop recording" };

    // Four rows of 46 px plus the heading and buttons fit the smallest
    // window the app opens at. Six rows did not: the buttons fell off the
    // bottom of the card and the take could not be stopped from it.
    static constexpr int kMaxRows = 4;
    static constexpr int kRowHeight = 46;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TakeAlertCard)
};

} // namespace mma
