#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Core/Metering.h"

namespace mma {

/// §9.1: the shared mix uses a horizontal bar, not a skull -- visually
/// distinct so the bus is never confused with a channel.
class MixBarComponent : public juce::Component, private juce::Timer
{
public:
    MixBarComponent();
    ~MixBarComponent() override;

    void setMetering (Metering* meteringSource) { metering = meteringSource; }

    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override;

    Metering* metering = nullptr;
    float currentLevelDb = Metering::kMinDb;
    float currentPeakDb = Metering::kMinDb;
    bool currentClip = false;

    static const juce::Colour kPanel;
    static const juce::Colour kEmptyInterior;
    static const juce::Colour kFillLow;
    static const juce::Colour kFillMid;
    static const juce::Colour kFillHigh;
    static const juce::Colour kBone;
    static const juce::Colour kSecondaryText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixBarComponent)
};

} // namespace mma
