#pragma once
#include "IAudioBackend.h"

#if JUCE_MAC

#include <memory>

namespace mma {

/// One open CoreAudio device stream: its AudioObjectID, IOProc registration and
/// the callback to forward into. Defined in the .cpp so this header stays free
/// of <CoreAudio/CoreAudio.h>.
struct CoreAudioStream;

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

    // Open streams, each owning its IOProc registration. Held as pointers so
    // the address handed to CoreAudio as clientData stays stable.
    std::vector<std::unique_ptr<CoreAudioStream>> openStreams;

    uint32_t openOutputDeviceId = 0;
    bool outputStreamIsHogModeExclusive = false;

    static std::vector<AudioDeviceDescriptor> enumerateDevices (bool wantInput);
    void installDeviceListListener();
    void removeDeviceListListener();
    bool openStream (const std::string& deviceId, double sampleRate, int bufferSizeSamples,
                     AudioCallback callback, bool isOutput);
};

} // namespace mma

#endif // JUCE_MAC
