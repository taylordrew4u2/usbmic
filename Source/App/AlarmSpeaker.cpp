#include "AlarmSpeaker.h"
#include <algorithm>

namespace mma {

AlarmSpeaker::AlarmSpeaker() = default;

AlarmSpeaker::~AlarmSpeaker()
{
    setActive (false);
}

bool AlarmSpeaker::setActive (bool shouldBeActive)
{
    if (shouldBeActive == active)
        return active;

    if (! shouldBeActive)
    {
        if (deviceManager != nullptr)
        {
            deviceManager->removeAudioCallback (this);
            deviceManager->closeAudioDevice();
            deviceManager.reset();
        }

        active = false;
        return false;
    }

    // Output only. Asking for inputs would raise the microphone permission
    // prompt for a device this never records from.
    deviceManager = std::make_unique<juce::AudioDeviceManager>();
    problem = deviceManager->initialiseWithDefaultDevices (0, 2);

    if (problem.isNotEmpty() || deviceManager->getCurrentAudioDevice() == nullptr)
    {
        if (problem.isEmpty())
            problem = "No default sound output was found.";

        deviceManager.reset();
        active = false;
        return false;
    }

    deviceManager->addAudioCallback (this);
    active = true;
    return true;
}

void AlarmSpeaker::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device != nullptr && device->getCurrentSampleRate() > 0.0)
        sampleRate = device->getCurrentSampleRate();
}

void AlarmSpeaker::audioDeviceStopped() {}

void AlarmSpeaker::audioDeviceIOCallbackWithContext (const float* const*, int,
                                                     float* const* outputChannelData, int numOutputChannels,
                                                     int numSamples,
                                                     const juce::AudioIODeviceCallbackContext&)
{
    if (numOutputChannels <= 0 || outputChannelData == nullptr || numSamples <= 0)
        return;

    // Silence, then the tone, on the first channel; the rest copy it.
    float* first = outputChannelData[0];

    if (first == nullptr)
        return;

    std::fill (first, first + numSamples, 0.0f);
    tone.render (first, numSamples, sampleRate);

    for (int ch = 1; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            std::copy (first, first + numSamples, outputChannelData[ch]);
}

} // namespace mma
