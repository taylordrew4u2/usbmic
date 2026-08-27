#pragma once
#include <memory>
#include <string>
#include <vector>
#include "../Platform/IAudioBackend.h"
#include "MonitorBus.h"
#include "Metering.h"
#include "WritePipeline.h"

namespace mma {

struct CaptureChannel
{
    std::string deviceId;
    std::string displayName;
    std::string fileName; // §6.2 sanitized, e.g. "01_Yeti-Kitchen"
    float trimDb = 0.0f;
};

/// Opens the audio streams and routes their callbacks. This is the piece that
/// makes the app actually hear and actually record: without it every subsystem
/// exists and nothing reaches a device.
///
/// It takes IAudioBackend by reference rather than constructing one, so the
/// whole path can be driven by a fake backend in tests. The platform backends'
/// own OS calls still need real hardware, but the wiring does not.
class CaptureCoordinator
{
public:
    CaptureCoordinator (IAudioBackend& backend, double sampleRate, int bufferSizeSamples);
    ~CaptureCoordinator();

    /// §5.1: monitoring is live from launch, independent of record state, so
    /// this opens one input stream per microphone plus the single output stream
    /// §5.2 permits. Returns false and names the cause if the exclusive-mode
    /// monitor path is unavailable (§5.4).
    bool startMonitoring (const std::vector<CaptureChannel>& channels,
                          const std::string& outputDeviceId);

    void stopMonitoring();

    bool isMonitoring() const noexcept { return monitoring; }
    const std::string& getMonitorProblem() const noexcept { return monitorProblem; }

    /// §6: begins writing. Monitoring continues untouched -- §5.1 makes the two
    /// independent, and §6.1 keeps a monitor mute from silencing the recording.
    bool startRecording (const std::string& sessionFolder, int bitDepth,
                         const std::string& originTimestamp);
    void stopRecording();
    bool isRecording() const noexcept { return pipeline != nullptr && pipeline->isRunning(); }

    /// §6.5: an unplugged mic keeps its channel and writes silence.
    void setChannelLive (const std::string& deviceId, bool live);

    /// The channel list this take was opened with. §6.5 fixes it for the
    /// duration of a recording, so callers reacting to a hot-plug must work
    /// from this rather than from the current device list.
    const std::vector<CaptureChannel>& getChannels() const noexcept { return channels; }

    MonitorBus& getMonitorBus() noexcept { return monitorBus; }
    Metering* getChannelMetering (int index) noexcept;
    Metering& getMixMetering() noexcept { return mixMeter; }

    uint64_t getFramesDropped() const noexcept { return pipeline != nullptr ? pipeline->getFramesDropped() : 0; }
    double getRingFillFraction() const noexcept { return pipeline != nullptr ? pipeline->getFillFraction() : 0.0; }

    /// The audio-thread entry point, public so tests can drive it directly.
    /// §11: no allocation, locking, logging or file I/O in here.
    void processAudioBlock (const float* const* inputs, int numInputs,
                            float* const* outputs, int numOutputs, int numSamples) noexcept;

private:
    IAudioBackend& backend;
    double sampleRate;
    int bufferSize;

    std::vector<CaptureChannel> channels;
    MonitorBus monitorBus;
    std::vector<std::unique_ptr<Metering>> channelMeters;
    Metering mixMeter;
    std::unique_ptr<WritePipeline> pipeline;

    bool monitoring = false;
    std::string monitorProblem;

    // Scratch for the summed monitor mix and the per-sample trim frame, both
    // sized at startMonitoring(). §11 forbids the callback allocating, and a
    // per-block vector here would do exactly that.
    std::vector<float> mixScratch;
    std::vector<float> trimFrame;
    std::vector<float> trimGains;
};

} // namespace mma
