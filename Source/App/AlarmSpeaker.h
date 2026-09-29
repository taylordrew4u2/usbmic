#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include "../Core/AlarmTone.h"
#include <memory>

namespace mma {

/// Where the app's own sounds go when there is no monitor output to carry
/// them: the computer's default output, opened in ordinary shared mode. A rig
/// with no headphones plugged into the app still has a laptop speaker, and a
/// siren nobody hears is no siren.
///
/// Only ever open while the coordinator has no output stream of its own. The
/// two must never be open together: on a machine where the default output IS
/// the monitor device, a second open would fight the exclusive stream for it.
class AlarmSpeaker : private juce::AudioIODeviceCallback
{
public:
    AlarmSpeaker();
    ~AlarmSpeaker() override;

    /// Opens the default output, or closes it. Idempotent. Returns whether an
    /// output is open afterwards.
    bool setActive (bool shouldBeActive);
    bool isActive() const noexcept { return active; }

    /// Why the last open failed, empty when it did not.
    juce::String getProblem() const { return problem; }

    AlarmTone& getAlarm() noexcept { return tone; }
    const AlarmTone& getAlarm() const noexcept { return tone; }

private:
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    std::unique_ptr<juce::AudioDeviceManager> deviceManager;
    AlarmTone tone;
    bool active = false;
    juce::String problem;
    double sampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AlarmSpeaker)
};

} // namespace mma
