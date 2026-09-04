#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include "../Platform/IAudioBackend.h"
#include "MonitorBus.h"
#include "DeviceInputStream.h"
#include "ChannelLayoutAnalyzer.h"
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

    /// The loudest sample the current take has written, or -1 when there is no
    /// pipeline to have measured one. Negative means "not measured" to
    /// judgeTakeAudio, which never reports silence on a reading nobody took.
    float getPeakWritten() const noexcept { return pipeline != nullptr ? pipeline->getPeakWritten() : -1.0f; }

    /// The loudest sample that reached this coordinator, whether or not the
    /// writer accepted it. Paired with getPeakWritten() it separates "no audio
    /// arrived" from "audio arrived and this app dropped it".
    float getPeakArrived() const noexcept { return peakArrived.load (std::memory_order_relaxed); }

    /// Frames the writer could not take. Zero on a healthy take.
    uint64_t getFramesAcceptedCount() const noexcept
    { return pipeline != nullptr ? pipeline->getFramesAccepted() : 0; }

    void resetArrivalPeak() noexcept { peakArrived.store (0.0f, std::memory_order_relaxed); }

    /// §6.5: shed the stems and keep the mix when the ring is nearly full and
    /// there is no mirror to fall back on.
    void fallBackToMixOnly() noexcept { if (pipeline != nullptr) pipeline->fallBackToMixOnly(); }
    bool isMixOnly() const noexcept { return pipeline != nullptr && pipeline->isMixOnly(); }

    /// Frames taken from the audio thread so far -- §6.5 wants the exact sample
    /// position where degradation began, and this is that clock.
    uint64_t getFramesAccepted() const noexcept { return pipeline != nullptr ? pipeline->getFramesAccepted() : 0; }

        /// §6.5 "target card removed": the destination stopped accepting writes.
    /// The take is over -- the owner stops and finalizes, and tells the user.
    bool hasCardWriteFailed() const noexcept { return pipeline != nullptr && pipeline->hasCardWriteFailed(); }

    /// §6.3: the mirror's equivalent. The pipeline already stops mirroring on
    /// a failed write and deliberately leaves the card write alone -- what this
    /// exposes is the fact that it happened, so the take's owner can say so and
    /// put it in the record.
    bool hasMirrorWriteFailed() const noexcept { return pipeline != nullptr && pipeline->hasMirrorWriteFailed(); }
    double getRingFillFraction() const noexcept { return pipeline != nullptr ? pipeline->getFillFraction() : 0.0; }

    /// BS.1770 loudness of the mix as written. What every streaming platform
    /// normalises against, and the only figure that says how loud a take will
    /// actually sound -- peak level says nothing about it.
    double getIntegratedLufs() const;
    double getTruePeakDbtp() const;
    int getLoudnessBlockCount() const;

    /// One microphone's audio callback (§3.2). Separate USB devices run on
    /// independent clocks, so each one delivers on its own thread and into its
    /// own ring rather than as one aligned block.
    void pushDeviceBlock (int deviceIndex, const float* samples, int numSamples) noexcept;

    /// §2.1: one microphone's callback when the device presents more than one
    /// channel, which many USB microphones do -- as stereo with one silent side,
    /// or with a single capsule duplicated across both.
    ///
    /// Picks the side the signal is actually on and pushes that. Taking channel
    /// 0 regardless is how a microphone wired to the right records silence:
    /// nothing warns, the meter sits at the floor, and the stem is empty.
    ///
    /// Real-time safe: the analysis is two passes over the block with no
    /// allocation, and the chosen channel is pushed by pointer, so collapsing to
    /// mono costs no copy (§11).
    void pushDeviceBlockMultiChannel (int deviceIndex, const float* const* inputs,
                                      int numInputs, int numSamples) noexcept;

    /// §2.1: which channel of the device this take is taking as mono, and what
    /// the analyzer concluded. -1 for a device that presented only one channel.
    int getChannelLayoutSource (int index) const noexcept;
    ChannelLayoutDecision getChannelLayoutDecision (int index) const noexcept;

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

    /// §3.1 / §3.3: which channel is the clock reference. Out-of-range clears it.
    ///
    /// This does not change what the capture path does to the audio. The stream
    /// every device is pulled by is the output device's (§3.2), so all channels
    /// are corrected onto that regardless -- exempting the master would leave it
    /// uncorrected against a clock it has no relationship to, not make it the
    /// timebase. What the reference selects is the channel §3.3's drift figures
    /// are quoted against, and the clock source Application hands the OS
    /// aggregate. Both are safe to move mid-take.
    void setMasterChannel (int index) noexcept;
    int getMasterChannel() const noexcept { return masterChannel; }

    /// §14.4: the strongest inter-channel correlation seen across the rig, and
    /// the quietest channel outside that pair.
    ///
    /// Two microphones hearing the same thing while a third hears nothing is
    /// what an omni or stereo pattern looks like from the outside -- the pair
    /// are picking up the room rather than the person in front of them. Only
    /// the audio path can measure it: correlation is a sample-level quantity
    /// and the per-channel peaks the advisor is otherwise fed cannot carry it.
    ///
    /// Meaningless below three channels, where §14.4's rule has no third
    /// channel to check, and reported as zero correlation so it cannot trigger.
    float getPolarPairCorrelation() const noexcept
    {
        return polarCorrelation.load (std::memory_order_relaxed);
    }

    float getPolarThirdChannelPeakDb() const noexcept
    {
        return polarThirdPeakDb.load (std::memory_order_relaxed);
    }

    /// §3.3 drift reporting, driven from the UI tick rather than the callback.
    void tickDriftReporting (double elapsedSeconds) noexcept;

    /// §3.3, relative to the clock master: positive means this device runs fast
    /// against it. The master reports zero against itself.
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

    /// Loudest sample seen arriving, across the whole take.
    std::atomic<float> peakArrived { 0.0f };

    /// §2.1 per device, for the ones that arrive with more than one channel.
    ///
    /// Touched only from that device's own audio callback, which is the single
    /// producer for it, so no lock is needed. `source` is read by the UI too and
    /// is therefore atomic -- a plain int written on the audio thread and read
    /// on the message thread is a data race even when every value is valid.
    struct ChannelLayout
    {
        ChannelLayoutAnalyzer analyzer { 48000.0 };
        std::atomic<int> source { -1 };
        std::atomic<int> decision { static_cast<int> (ChannelLayoutDecision::Pending) };
        bool frozen = false;
    };

    std::vector<std::unique_ptr<ChannelLayout>> channelLayouts;
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

    // §14.4, same ownership: written in the callback, read on the UI tick.
    //
    // The floor is a "nothing heard yet" value, not a loud one: a peak-hold
    // that starts at 0 dBFS spends its first seconds decaying down from a level
    // nothing produced, which would disarm the detector exactly when a take is
    // starting. Nothing can trigger from it regardless, because the correlation
    // beside it starts at zero.
    static constexpr float kPolarFloorDb = -200.0f;

    std::atomic<float> polarCorrelation { 0.0f };
    std::atomic<float> polarThirdPeakDb { kPolarFloorDb };

    // Peak-held on the audio thread so a loud moment on the third channel
    // survives until the UI next looks. The tick runs at 2 Hz and sees one
    // block in several hundred; without the hold, the one thing that should
    // break a false trigger is the thing most likely to be missed.
    float polarThirdPeakHeldDb = kPolarFloorDb;

    /// §14.4 measurement over one already-aligned frame block. Real-time safe:
    /// sums over the block, no allocation, no locking.
    void measurePolarPattern (const float* const* inputs, int channelCount, int numSamples) noexcept;

    void noteCallbackLoad (std::chrono::steady_clock::time_point start, int numSamples) noexcept;

    /// The reference channel's own correction against the output clock, which
    /// every §3.3 figure is quoted relative to. Zero when there is no master.
    double getMasterDriftPpm() const noexcept;

    /// Shared by both capture paths: sum, meter, record and publish one already
    /// time-aligned frame block. Real-time safe.
    void mixAndPublish (const float* const* inputs, int channelCount,
                        float* const* outputs, int numOutputs, int numSamples) noexcept;
};

} // namespace mma
