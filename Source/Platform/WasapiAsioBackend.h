#pragma once
#include "IAudioBackend.h"

#if JUCE_WINDOWS

namespace mma {

/// Windows implementation of IAudioBackend. Prefers ASIO drivers when
/// present (lowest, most predictable latency, and JUCE's AudioIODeviceType
/// "ASIO" wraps driver enumeration for us); falls back to WASAPI in
/// EXCLUSIVE mode only. §5.4: shared-mode WASAPI is explicitly disqualified
/// for the monitor path regardless of which app-facing backend (§7) is
/// active -- it is never selected here even as a last resort, because a
/// silent 40-100ms mix is worse than telling the user why low-latency
/// monitoring isn't available.
class WasapiAsioBackend : public IAudioBackend
{
public:
    WasapiAsioBackend();
    ~WasapiAsioBackend() override;

    std::string getBackendName() const override { return preferAsio ? "ASIO" : "WASAPI (exclusive)"; }

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
    bool preferAsio = false;
    DeviceChangeCallback deviceChangeCallback;

    // Opaque IMMNotificationClient registration handle; the concrete COM
    // object is defined in the .cpp to avoid pulling <mmdeviceapi.h> into
    // every translation unit that includes this header.
    void* notificationClient = nullptr;

    bool hasAnyAsioDriverInstalled() const;
    std::vector<AudioDeviceDescriptor> enumerateWasapiDevices (bool wantInput) const;
    std::vector<AudioDeviceDescriptor> enumerateAsioDevices() const;

    /// Opens a WASAPI stream in AUDCLNT_SHAREMODE_EXCLUSIVE. Never opens
    /// shared mode for the monitor path -- see class doc.
    bool openWasapiExclusiveStream (const std::string& deviceId, double sampleRate,
                                    int bufferSizeSamples, bool isInput, AudioCallback callback);
};

} // namespace mma

#endif // JUCE_WINDOWS
