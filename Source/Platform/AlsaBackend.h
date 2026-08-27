#pragma once

// Guarded on __linux__, not a JUCE macro. The compiler defines this one itself,
// so unlike JUCE_MAC / JUCE_WINDOWS it cannot silently evaluate to 0 and leave
// an empty object file -- which is exactly how both shipping platforms once
// ended up unable to link. It also keeps this backend out of the JUCE
// dependency, so the headless test build can drive a real OS audio API.
//
// MMA_NO_ALSA is set by CMake when the ALSA development headers are absent, so
// a Linux box without them still builds everything else.
#if defined(__linux__) && ! defined(MMA_NO_ALSA)

#include "IAudioBackend.h"
#include <memory>
#include <vector>

namespace mma {

/// One open ALSA stream: its PCM handle, worker thread and conversion scratch.
/// Defined in the .cpp so <alsa/asoundlib.h> stays out of this header.
struct AlsaStream;

/// Linux implementation of IAudioBackend, on ALSA directly rather than through
/// JUCE, for the same reason as the other two: §5.4 needs control over whether
/// the device is opened exclusively, and §2 needs device-change notification
/// that is not a timer.
///
/// §5.4 on ALSA: a `hw:` device is exclusive by construction -- the kernel
/// hands it to one client. `default`, `plughw:` and `dmix` are the shared,
/// resampled paths and are reported as non-exclusive, so the monitor path
/// refuses them rather than silently delivering a mixed, high-latency stream.
class AlsaBackend : public IAudioBackend
{
public:
    AlsaBackend();
    ~AlsaBackend() override;

    std::string getBackendName() const override { return "ALSA"; }

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
    std::vector<std::unique_ptr<AlsaStream>> openStreams;

    /// inotify watch on /dev/snd, so hotplug arrives from the kernel rather
    /// than a poll (§2). Its own thread; -1 when unavailable.
    int inotifyFd = -1;
    int inotifyWatch = -1;
    std::unique_ptr<struct AlsaHotplugWatcher> hotplug;

    std::vector<AudioDeviceDescriptor> enumerate (bool wantInput) const;
    bool openStream (const std::string& deviceId, double sampleRate, int bufferSizeSamples,
                     bool isInput, AudioCallback callback);
};

} // namespace mma

#endif // __linux__ && ! MMA_NO_ALSA
