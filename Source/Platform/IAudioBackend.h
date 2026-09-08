#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <mutex>
#include <utility>

namespace mma {

struct AudioDeviceDescriptor
{
    std::string name;          // e.g. "Blue Yeti"
    std::string usbLocationId; // §2.4 identity
    std::string serialNumber;  // may be empty
    bool isBuiltIn = false;    // the machine's own mic, not something plugged in
    int maxInputChannels = 0;
    std::vector<uint32_t> supportedSampleRates;
    /// The rate the device is running at now, or 0 when the backend cannot say.
    /// Advertising a rate is not the same as being willing to switch to it.
    uint32_t currentSampleRate = 0;
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

/// A stream that died after it had been opened. See takeStreamFailures().
struct StreamFailure
{
    /// The device the stream was opened for. Empty for the monitor output.
    std::string deviceId;

    /// §10.6: what happened, in the user's words. The owner adds the device's
    /// name -- the backend only knows its id.
    std::string reason;
};

/// Where a backend's worker threads leave a failure for the message thread to
/// pick up. Locked rather than lock-free: a stream dying is a once-per-session
/// event, and the alternative -- a fixed slot that the second failure
/// overwrites -- loses exactly the information this exists to keep.
///
/// note() is called from a worker thread that has already stopped feeding the
/// audio callback, so it is not on the real-time path and may lock.
class StreamFailureSink
{
public:
    void note (std::string deviceId, std::string reason)
    {
        const std::lock_guard<std::mutex> guard (lock);

        // A device that fails repeatedly says it once. The owner turns this
        // into a sentence for the user, not a counter.
        for (const auto& existing : failures)
            if (existing.deviceId == deviceId)
                return;

        failures.push_back ({ std::move (deviceId), std::move (reason) });
    }

    std::vector<StreamFailure> take()
    {
        const std::lock_guard<std::mutex> guard (lock);
        return std::exchange (failures, {});
    }

    void clear()
    {
        const std::lock_guard<std::mutex> guard (lock);
        failures.clear();
    }

private:
    std::mutex lock;
    std::vector<StreamFailure> failures;
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

    /// A stream that opened successfully and has since stopped delivering
    /// audio on its own, with the reason, taken and cleared.
    ///
    /// Every backend has a worker loop that gives up on an unrecoverable device
    /// error and exits, and none of them told anyone. The stream simply stopped:
    /// monitoring went quiet, or a microphone's track went on being written as
    /// silence for the rest of a four-hour take, and the only evidence was in
    /// the file afterwards. §0.1 does not allow that, so a backend that stops
    /// says so, and the owner turns it into a sentence.
    ///
    /// Taken rather than read, so each failure is reported once. Empty
    /// deviceId means the monitor output rather than an input.
    virtual std::vector<StreamFailure> takeStreamFailures() { return {}; }

    /// §0.1: frames that reached the backend from a device and were never
    /// handed to the audio callback, cumulative since the streams opened.
    ///
    /// WritePipeline already counts what it drops on the way to disk, and that
    /// count is reported. Everything lost BEFORE the callback was invisible to
    /// it -- a packet the device refused to hand over, or one wider than the
    /// scratch the stream allocated -- so audio could be lost between the
    /// microphone and the meter with nothing anywhere saying so.
    virtual uint64_t getFramesDroppedByBackend() const { return 0; }

    virtual void closeAllStreams() = 0;
};

} // namespace mma
