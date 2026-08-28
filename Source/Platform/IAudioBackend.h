#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace mma {

struct AudioDeviceDescriptor
{
    std::string name;          // e.g. "Blue Yeti"
    std::string usbLocationId; // §2.4 identity
    std::string serialNumber;  // may be empty
    int maxInputChannels = 0;
    std::vector<uint32_t> supportedSampleRates;
    std::vector<int> supportedBitDepths;
    bool isMicrophone = false;
    bool hasPhysicalHeadphoneJack = false; // relevant for output-device candidates (§5.3)
};

/// §5.4: the monitor path requires exclusive-mode audio. This describes what a
/// candidate device/mode combination can actually deliver.
struct ExclusiveModeCapability
{
    bool exclusiveModeAvailable = false;
    double measuredOrEstimatedLatencyMs = 0.0;
    std::string unavailableReason; // populated when exclusiveModeAvailable is false
};

using AudioCallback = std::function<void (const float* const* inputChannels, int numInputChannels,
                                          float* const* outputChannels, int numOutputChannels,
                                          int numSamples)>;
using DeviceChangeCallback = std::function<void()>;

/// Platform audio backend interface (§11: CoreAudio on macOS, WASAPI
/// exclusive + ASIO on Windows). Implementations enumerate input devices,
/// open exclusive-mode streams for the monitor path, and deliver a
/// synchronous audio callback per §11 ("no allocation, locking, logging,
/// file I/O, or drawing" inside it).
class IAudioBackend
{
public:
    virtual ~IAudioBackend() = default;

    virtual std::string getBackendName() const = 0;

    /// Enumerate all USB audio input devices, called at launch and again on
    /// every OS device-change notification (§2, never on a timer).
    virtual std::vector<AudioDeviceDescriptor> enumerateInputDevices() = 0;

    virtual std::vector<AudioDeviceDescriptor> enumerateOutputDevices() = 0;

    /// Registers a callback the backend invokes on its own OS device-change
    /// notification mechanism (IONotification/CoreAudio listener on macOS,
    /// WM_DEVICECHANGE/MMNotificationClient on Windows).
    virtual void setDeviceChangeCallback (DeviceChangeCallback callback) = 0;

    /// Checks whether exclusive mode is available for the given output device
    /// at the given sample rate/buffer size, per §5.4.
    virtual ExclusiveModeCapability checkExclusiveModeCapability (const std::string& outputDeviceId,
                                                                  double sampleRate, int bufferSizeSamples) = 0;

    /// Opens the monitor output stream in exclusive mode. Returns false (and
    /// should log/report why) if exclusive mode can't be obtained -- per
    /// §5.4 "never ship a 40ms mix silently".
    virtual bool openExclusiveOutputStream (const std::string& outputDeviceId, double sampleRate,
                                            int bufferSizeSamples, AudioCallback callback) = 0;

    /// A user-facing explanation of why the most recent
    /// openExclusiveOutputStream call returned false, or "" if the backend has
    /// nothing more specific to add. Kept separate from the return value so
    /// existing backends need not implement it, and so the message can name a
    /// next step rather than leaving the user at a dead end.
    virtual std::string getLastOpenError() const { return {}; }

    /// Opens one input device's capture stream.
    virtual bool openInputStream (const std::string& inputDeviceId, double sampleRate,
                                  int bufferSizeSamples, AudioCallback callback) = 0;

    virtual void closeAllStreams() = 0;
};

} // namespace mma
