#pragma once
#include "ModalCard.h"
#include <functional>
#include <memory>
#include <vector>

namespace mma {

/// §0.1 / §6.5: the pop-up for something going wrong in the middle of a take.
/// A microphone unplugged, a camera switched off, sound dropped, the drive
/// falling behind or running out of room. Each is one line, newest at the
/// top, with when in the take it happened. The take carries on behind the
/// card either way; the two buttons are "carry on" and "stop now".
class TakeAlertCard : public ModalCard,
                      private juce::Timer
{
public:
    /// How much a line matters, which decides its colour bar and how the
    /// heading reads. Every bad line used to be the same amber, so a
    /// microphone that had gone and a camera that had gone looked equally
    /// alarming -- and one of them had not touched the audio at all.
    enum class Tone
    {
        NeedsYou,   ///< Do something now or the take suffers.
        Watch,      ///< Worth knowing; the take carries on regardless.
        Fixed,      ///< Something that was lost has come back.
        Quiet,      ///< Real news, but the recorded audio was never at risk.
    };

    TakeAlertCard();
    ~TakeAlertCard() override;

    /// Adds a line, newest at the top.
    void addAlert (const juce::String& whenInTake, const juce::String& message, Tone tone);
    void clear();

    /// Red heading, "Recording failed" wording: for a take that has stopped
    /// itself or a drive that has stopped taking audio. Back to the calm
    /// heading on clear().
    void setSevere (bool severe, bool takeStopped);
    int getAlertCount() const { return static_cast<int> (rows.size()); }

    std::function<void()> onKeepRecording;
    std::function<void()> onStopRecording;

    void paint (juce::Graphics& g) override;
    bool keyPressed (const juce::KeyPress& key) override;
    /// The keyboard goes to Keep recording, or to OK when a stopped take has
    /// hidden it -- never left behind the card on the record button.
    void prepareToShow()
    {
        if (keepButton.isVisible())
            keepButton.grabKeyboardFocus();
        else
            stopButton.grabKeyboardFocus();
    }

    /// The alarm: the backdrop flashes red, a banner across the card says
    /// SOMETHING IS WRONG in letters the whole room can read, and the owner
    /// keeps the siren going in the headphones for as long as this is true.
    /// Cleared by Keep recording, Stop recording, or the card being hidden.
    void setAlarming (bool shouldAlarm);
    bool isAlarming() const noexcept { return alarming; }
    void setReducedMotion (bool shouldReduceMotion) { reducedMotion = shouldReduceMotion; }
    juce::String getBannerText() const;

    /// Fast enough to be impossible to miss, slow enough to read the card
    /// through. Reduced motion pulses once a second instead.
    static constexpr int kFlashMs = 250;
    static constexpr int kReducedFlashMs = 1000;
    static constexpr int kBannerHeight = 96;

    void visibilityChanged() override;

protected:
    int getContentHeight() const override;
    void layOutContent (juce::Rectangle<int> area) override;

private:
    struct Row
    {
        std::unique_ptr<juce::Label> when, text;
        Tone tone = Tone::Watch;
        juce::Rectangle<int> bar;
    };

    std::vector<Row> rows;

    /// Lines that fell off the bottom. They used to vanish without trace,
    /// which is the same silence the rest of the app spent so long removing:
    /// now the card says how many there were and where to read them.
    int rowsDropped = 0;
    std::unique_ptr<juce::Label> overflowLabel;

    bool severeHeading = false;
    bool severeTakeStopped = false;
    bool alarming = false;
    bool reducedMotion = false;
    double alarmStartedMs = 0.0;
    juce::Rectangle<int> bannerBounds;
    void timerCallback() override;
    bool litNow() const;

    juce::TextButton keepButton { "Keep recording" };
    juce::TextButton stopButton { "Stop recording" };

    // Four rows of 46 px plus the heading and buttons fit the smallest
    // window the app opens at. Six rows did not: the buttons fell off the
    // bottom of the card and the take could not be stopped from it.
    static constexpr int kMaxRows = 4;
    static constexpr int kRowHeight = 46;
    static constexpr int kOverflowHeight = 20;
    static constexpr int kBarWidth = 3;
    static constexpr int kWhenWidth = 54;

    static juce::Colour colourFor (Tone tone);
    void refreshCalmHeading();
    void refreshOverflowLabel();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TakeAlertCard)
};

} // namespace mma
