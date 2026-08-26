#pragma once
#include "IAudioBackend.h"

#if JUCE_MAC

namespace mma {

/// macOS implementation of IAudioBackend using CoreAudio directly (not JUCE's
/// generic AudioIODeviceType) so we get exclusive/hog-mode control and raw
/// AudioObjectID-level device change notifications per §5.4/§11. Every USB
/// mic on macOS is a HAL AudioObjectID; hotplug arrives via
/// kAudioHardwarePropertyDevices property listeners, never a timer (§2).
class CoreAudioBackend : public IAudioBackend
{
public:
    CoreAudioBackend();
    ~CoreAudioBackend() override;

    std::string getBackendName() const override { return "CoreAudio"; }

    std::vector<AudioDeviceDescriptor> enumerateInputDevices() override;
    std::vector<AudioDeviceDescriptor> enumerateOutputDevices() override;
    void setDeviceChangeCallback (DeviceChangeCallback callback) override;

    ExclusiveModeCapability checkExclusiveModeCapability (const std::string& outputDeviceId,
                                                          double sampleRate, int bufferSizeSamples) override;

    bool openExclusiveOutputStream (const std::string& outputDeviceId, double sampleRate,
                                    int bufferSizeSamples, AudioCallback callback) override;

    bool openInputStream (const std::string& inputDeviceId, double sampleRate,
                          int bufferSizeSamples, AudioCallback callback) override;

    void closeAllStreams() override;

private:
    DeviceChangeCallback deviceChangeCallback;

    // AudioObjectID handles for open input streams and the output stream, and
    // the AudioDeviceIOProc callback trampolines that forward into the
    // AudioCallback given to open*Stream(). Held as opaque uint32_t here to
    // avoid dragging <CoreAudio/CoreAudio.h> into this header; the .cpp
    // includes it directly.
    std::vector<uint32_t> openInputDeviceIds;
    uint32_t openOutputDeviceId = 0;
    bool outputStreamIsHogModeExclusive = false;

    static std::vector<AudioDeviceDescriptor> enumerateDevices (bool wantInput);
    void installDeviceListListener();
    void removeDeviceListListener();
};

} // namespace mma

#endif // JUCE_MAC
