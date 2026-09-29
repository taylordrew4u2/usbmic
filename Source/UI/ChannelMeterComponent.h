#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Core/Metering.h"
#include <functional>

namespace mma {

/// §9: one microphone's channel strip -- a ring badge that fills with the
/// level, the mic name, a level track with a peak tick, and the dBFS number.
/// The badge wears the app icon's crying face. Clip turns the ring and the
/// number `clipEyes` and reads "CLIP n".
class ChannelMeterComponent : public juce::Component, private juce::Timer
{
public:
    ChannelMeterComponent();
    ~ChannelMeterComponent() override;

    void setMetering (Metering* meteringSource) { metering = meteringSource; }
    void setMicName (const juce::String& name)
    {
        if (micName != name) { micName = name; updateAccessibilityText(); repaint(); }
    }
    void setDeviceName (const juce::String& name)
    {
        if (deviceName != name) { deviceName = name; updateAccessibilityText(); repaint(); }
    }
    void setNoSignal (bool isNoSignal)
    {
        if (noSignal != isNoSignal) { noSignal = isNoSignal; updateAccessibilityText(); repaint(); }
    }

    /// §14.6: lit while this mic is the one being heard, so a user with four
    /// identical mics can see which strip is which person.
    void setHighlighted (bool shouldHighlight);

    /// Fired on click when there is no clip latch to acknowledge -- the rename
    /// affordance. Clearing a clip stays the first click's job (§9.1).
    std::function<void()> onNameClicked;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent& event) override;
    bool keyPressed (const juce::KeyPress& key) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void timerCallback() override;

    Metering* metering = nullptr;
    juce::String micName, deviceName;
    bool noSignal = true;
    bool highlighted = false;

    float currentLevelDb = Metering::kMinDb;
    float currentPeakDb = Metering::kMinDb;
    bool currentClip = false;
    // §9.3: the count is what makes clip a number rather than only a hue, so it
    // is read alongside the latch rather than derived from it.
    int currentClipCount = 0;
    int lastAccessibleLevelDb = -1000;

    // §9.2 palette.
    static const juce::Colour kPanel;
    static const juce::Colour kBone;
    static const juce::Colour kEmptyInterior;
    static const juce::Colour kFillLow;
    static const juce::Colour kFillMid;
    static const juce::Colour kFillHigh;
    static const juce::Colour kClipEyes;
    static const juce::Colour kDimmedOutline;
    static const juce::Colour kTertiaryText;

    void paintStrip (juce::Graphics& g, juce::Rectangle<float> bounds);

    /// The icon's face inside the badge: eyes and a frown, a tear once the
    /// level is loud, two on a clip, closed eyes with no signal.
    void paintFace (juce::Graphics& g, juce::Rectangle<float> badge, float norm);
    static juce::Path teardrop (juce::Point<float> tip, float height);
    juce::Colour fillColourForLevel (float levelDb) const;
    void performPrimaryAction();
    void updateAccessibilityText();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelMeterComponent)
};

} // namespace mma
