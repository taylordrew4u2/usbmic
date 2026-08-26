#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Core/Metering.h"

namespace mma {

/// §9: the skull-shaped meter. The skull silhouette IS the meter -- level
/// fills it from the jaw upward, eye sockets are the clip indicator, peak
/// hold is a bone-white bar. Palette values are §9.2 verbatim.
class SkullMeterComponent : public juce::Component, private juce::Timer
{
public:
    SkullMeterComponent();
    ~SkullMeterComponent() override;

    void setMetering (Metering* meteringSource) { metering = meteringSource; }
    void setMicName (const juce::String& name) { micName = name; repaint(); }
    void setDeviceName (const juce::String& name) { deviceName = name; repaint(); }
    void setNoSignal (bool isNoSignal) { noSignal = isNoSignal; repaint(); }
    void setReducedMotion (bool shouldReduceMotion) { reducedMotion = shouldReduceMotion; }

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent& event) override;

private:
    void timerCallback() override;

    Metering* metering = nullptr;
    juce::String micName, deviceName;
    bool noSignal = true;
    bool reducedMotion = false;

    float currentLevelDb = Metering::kMinDb;
    float currentPeakDb = Metering::kMinDb;
    bool currentClip = false;

    // §9.2 palette, verbatim.
    static const juce::Colour kBackground;
    static const juce::Colour kPanel;
    static const juce::Colour kBone;
    static const juce::Colour kEmptyInterior;
    static const juce::Colour kFillLow;
    static const juce::Colour kFillMid;
    static const juce::Colour kFillHigh;
    static const juce::Colour kClipEyes;
    static const juce::Colour kDimmedOutline;
    static const juce::Colour kSecondaryText;
    static const juce::Colour kTertiaryText;

    juce::Path buildSkullSilhouette (juce::Rectangle<float> bounds) const;
    juce::Colour fillColourForLevel (float levelDb) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SkullMeterComponent)
};

} // namespace mma
