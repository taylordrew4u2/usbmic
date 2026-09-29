#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace mma {

/// The whole window, taken over for three seconds, to say a take has started
/// or stopped. Red and flashing for a start, cyan and flashing for a stop,
/// in letters that can be read from the back of the room, with the take's
/// name under them. Nobody on a rig should ever have to squint at a button
/// to know whether they are being recorded.
///
/// It takes no clicks and no keys: whatever is behind it -- the saved-take
/// card, the main screen -- keeps working through it, so it can never hold
/// anyone up. A reduced-motion preference keeps the takeover but drops the
/// flashing.
class TakeBanner : public juce::Component,
                   private juce::Timer
{
public:
    enum class Kind { Started, Stopped };

    static constexpr int kHoldMs = 3000;
    static constexpr int kFlashMs = 250;

    TakeBanner();
    ~TakeBanner() override;

    void show (Kind kind, const juce::String& title, const juce::String& detail);
    void setReducedMotion (bool shouldReduceMotion) { reducedMotion = shouldReduceMotion; }

    Kind getKind() const noexcept { return kind; }
    juce::String getTitle() const { return title; }

    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override;
    bool litNow() const;

    Kind kind = Kind::Started;
    juce::String title, detail;
    double shownAtMs = 0.0;
    bool reducedMotion = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TakeBanner)
};

} // namespace mma
