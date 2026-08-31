#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include "../Platform/IAudioBackend.h"
#include "MonitorBus.h"
#include "DeviceInputStream.h"
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
    /// mirrorFolder is §6.3's local copy; empty means card-only.
    bool startRecording (const std::string& sessionFolder, int bitDepth,
                         const std::string& originTimestamp,
                         const std::string& mirrorFolder = {});

    /// §6.3: the internal drive ran low mid-take. Stops the copy and keeps the
    /// card write going.
    void stopMirroring();
    bool isMirroring() const noexcept { return pipeline != nullptr && pipeline->isMirroring(); }
    void stopRecording();
    bool isRecording() const noexcept { return pipeline != nullptr && pipeline->isRunning(); }

    /// §6.5: an unplugged mic keeps its channel and writes silence.
    void setChannelLive (const std::string& deviceId, bool live);

    /// §4: trim, live. Applies to the monitor mix and the mix file; the stems
    /// stay at unity either way. Safe to call while the callback is running --
    /// it stores one float per channel into an already-sized vector.
    void setChannelTrimDb (int index, float trimDb) noexcept;
    float getChannelTrimDb (int index) const noexcept;

    /// The channel list this take was opened with. §6.5 fixes it for the
    /// duration of a recording, so callers reacting to a hot-plug must work
    /// from this rather than from the current device list.
    const std::vector<CaptureChannel>& getChannels() const noexcept { return channels; }

    MonitorBus& getMonitorBus() noexcept { return monitorBus; }
    Metering* getChannelMetering (int index) noexcept;
    Metering& getMixMetering() noexcept { return mixMeter; }

    /// §6.6: the fraction of each callback's available time actually spent in
    /// it. This is the load that matters for dropouts -- overall machine CPU
    /// can look calm while the audio thread is already missing its deadline.
    double getAudioCallbackLoad() const noexcept { return callbackLoad.load (std::memory_order_relaxed); }

    uint64_t getFramesDropped() const noexcept { return pipeline != nullptr ? pipeline->getFramesDropped() : 0; }

    /// §6.5 "target card removed": the destination stopped accepting writes.
    /// The take is over -- the owner stops and finalizes, and tells the user.
    bool hasCardWriteFailed() const noexcept { return pipeline != nullptr && pipeline->hasCardWriteFailed(); }
    double getRingFillFraction() const noexcept { return pipeline != nullptr ? pipeline->getFillFraction() : 0.0; }

    /// One microphone's audio callback (§3.2). Separate USB devices run on
    /// independent clocks, so each one delivers on its own thread and into its
    /// own ring rather than as one aligned block.
    void pushDeviceBlock (int deviceIndex, const float* samples, int numSamples) noexcept;

    /// The output device's callback, which is the clock everything else is
    /// pulled onto (§3.1). Sums the drift-corrected mics, meters them, feeds the
    /// writer, and fills the headphone buffers.
    void processOutputBlock (float* const* outputs, int numOutputs, int numSamples) noexcept;

    /// Aggregate-device path: every channel arriving in one already-aligned
    /// callback, as a CoreAudio aggregate or an ASIO device delivers it. No
    /// drift correction is applied because the OS has already done it.
    /// §11: no allocation, locking, logging or file I/O in here.
    void processAudioBlock (const float* const* inputs, int numInputs,
                            float* const* outputs, int numOutputs, int numSamples) noexcept;

    /// §3.1 / §3.3: which channel defines the timebase. Every other device is
    /// resampled onto it; the master itself never is. Out-of-range clears it.
    void setMasterChannel (int index) noexcept;
    int getMasterChannel() const noexcept { return masterChannel; }

    /// §3.3 drift reporting, driven from the UI tick rather than the callback.
    void tickDriftReporting (double elapsedSeconds) noexcept;
    double getChannelDriftPpm (int index) const noexcept;
    bool hasSustainedExcessDrift (int index) const noexcept;
    uint64_t getUnderrunSamples (int index) const noexcept;

private:
    IAudioBackend& backend;
    double sampleRate;
    int bufferSize;

    std::vector<CaptureChannel> channels;
    MonitorBus monitorBus;
    std::vector<std::unique_ptr<Metering>> channelMeters;
    std::vector<std::unique_ptr<DeviceInputStream>> deviceStreams;
    int masterChannel = -1;
    Metering mixMeter;

    // Owned by the UI thread. The audio thread never reads this handle -- it
    // reads activePipeline below, which is a plain pointer it can load
    // atomically. A std::unique_ptr is three words with no atomicity guarantee
    // of any kind, so the callback testing `pipeline != nullptr` while
    // startRecording/stopRecording moved it was a data race that could hand the
    // audio thread a pointer to a pipeline being destroyed underneath it.
    std::unique_ptr<WritePipeline> pipeline;

    // The audio thread's view of the pipeline: published with release once the
    // pipeline is fully started, cleared with release before it is torn down.
    std::atomic<WritePipeline*> activePipeline { nullptr };

    // Non-zero while a callback is inside the region that dereferences
    // activePipeline. stopRecording waits for this to drain before destroying
    // the pipeline, so a callback that loaded the pointer just before the store
    // still finishes against a live object.
    std::atomic<int> pipelineUsers { 0 };

    bool monitoring = false;
    std::string monitorProblem;

    // Scratch for the summed monitor mix and the per-sample trim frame, both
    // sized at startMonitoring(). §11 forbids the callback allocating, and a
    // per-block vector here would do exactly that.
    std::vector<float> mixScratch;
    std::vector<float> trimFrame;
    std::vector<float> trimGains;

    // Per-device pulled audio and the pointer table the writer wants, both
    // sized at startMonitoring(). The output callback fills these every block
    // and §11 forbids it allocating them there.
    std::vector<float> deviceScratch;      // channelCount * bufferSize, contiguous per channel
    std::vector<const float*> devicePointers;

    // Written by the audio thread, read by the UI. Relaxed because a stale
    // reading for one frame is harmless and a lock here would not be (§11).
    std::atomic<double> callbackLoad { 0.0 };

    void noteCallbackLoad (std::chrono::steady_clock::time_point start, int numSamples) noexcept;

    /// Shared by both capture paths: sum, meter, record and publish one already
    /// time-aligned frame block. Real-time safe.
    void mixAndPublish (const float* const* inputs, int channelCount,
                        float* const* outputs, int numOutputs, int numSamples) noexcept;
};

} // namespace mma
