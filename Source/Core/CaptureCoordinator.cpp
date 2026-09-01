#include "CaptureCoordinator.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace mma {

CaptureCoordinator::CaptureCoordinator (IAudioBackend& b, double rate, int bufferSizeSamples)
    : backend (b), sampleRate (rate), bufferSize (bufferSizeSamples),
      monitorBus (rate), mixMeter (rate)
{
}

CaptureCoordinator::~CaptureCoordinator()
{
    stopRecording();
    stopMonitoring();
}

bool CaptureCoordinator::startMonitoring (const std::vector<CaptureChannel>& chans,
                                          const std::string& outputDeviceId)
{
    stopMonitoring();

    channels = chans;
    monitorProblem.clear();

    channelMeters.clear();
    for (size_t i = 0; i < channels.size(); ++i)
        channelMeters.push_back (std::make_unique<Metering> (sampleRate));

    // Sized here so the audio callback never allocates (§11).
    mixScratch.assign (static_cast<size_t> (std::max (1, bufferSize)) * 8, 0.0f);
    trimFrame.assign (std::max<size_t> (1, channels.size()), 0.0f);

    // §3.2: one capture path per device, each with its own ring and PI loop.
    deviceStreams.clear();
    channelLayouts.clear();
    for (size_t i = 0; i < channels.size(); ++i)
    {
        auto stream = std::make_unique<DeviceInputStream> (sampleRate);
        stream->prepare (sampleRate, bufferSize);
        deviceStreams.push_back (std::move (stream));

        // §2.1 starts afresh for each opened device: the analyzer's 60-second
        // timeout is measured from the moment the device is seen.
        auto layout = std::make_unique<ChannelLayout>();
        layout->analyzer = ChannelLayoutAnalyzer (sampleRate);
        channelLayouts.push_back (std::move (layout));
    }

    // §3.1: until the caller says otherwise, the first included mic is the
    // reference §3.3's drift figures are quoted against.
    setMasterChannel (channels.empty() ? -1 : 0);

    deviceScratch.assign (std::max<size_t> (1, channels.size())
                              * static_cast<size_t> (std::max (1, bufferSize)), 0.0f);
    devicePointers.assign (std::max<size_t> (1, channels.size()), nullptr);

    // Precomputed so the callback never calls a dB->linear conversion per sample.
    trimGains.clear();
    for (const auto& ch : channels)
        trimGains.push_back (MonitorBus::trimDbToLinearGain (ch.trimDb));

    // §5.4: the monitor path must be exclusive-mode. If it is not available,
    // say so and name the cause rather than silently delivering 40 ms.
    if (! outputDeviceId.empty())
    {
        const auto capability = backend.checkExclusiveModeCapability (outputDeviceId, sampleRate, bufferSize);

        if (! capability.exclusiveModeAvailable)
        {
            monitorProblem = capability.unavailableReason.empty()
                ? std::string ("Low-latency monitoring isn't available on this sound output.")
                : capability.unavailableReason;
            return false;
        }
    }

    // §5.2: exactly one output stream, ever. It is also the clock -- §3.1 needs
    // one timebase, and the device feeding the headphones is the one whose
    // deadline actually matters.
    auto outputCallback = [this] (const float* const*, int,
                                  float* const* outputs, int numOutputs, int numSamples)
    {
        processOutputBlock (outputs, numOutputs, numSamples);
    };

    if (! outputDeviceId.empty()
        && ! backend.openExclusiveOutputStream (outputDeviceId, sampleRate, bufferSize, outputCallback))
    {
        // Prefer whatever the backend can say about the specific device; the
        // generic line leaves the user with no next step.
        const auto backendReason = backend.getLastOpenError();
        monitorProblem = backendReason.empty()
            ? std::string ("Couldn't open your headphones for low-latency playback.")
            : backendReason;
        backend.closeAllStreams();
        return false;
    }

    for (size_t i = 0; i < channels.size(); ++i)
    {
        // Bound to its own index. Passing one shared callback to every device
        // was the bug this replaces: each device delivers only its own audio,
        // so a shared callback wrote every microphone into channel 0.
        const int index = static_cast<int> (i);

        auto inputCallback = [this, index] (const float* const* inputs, int numInputs,
                                            float* const*, int, int numSamples)
        {
            if (inputs == nullptr || numInputs <= 0)
                return;

            // §2.1: a device that presents more than one channel gets the side
            // it is actually using picked for it. One that presents a single
            // channel has nothing to decide.
            if (numInputs >= 2)
                pushDeviceBlockMultiChannel (index, inputs, numInputs, numSamples);
            else
                pushDeviceBlock (index, inputs[0], numSamples);
        };

        if (! backend.openInputStream (channels[i].deviceId, sampleRate, bufferSize, inputCallback))
        {
            monitorProblem = channels[i].displayName + " couldn't be opened for recording.";
            backend.closeAllStreams();
            return false;
        }
    }

    monitoring = true;
    return true;
}

void CaptureCoordinator::stopMonitoring()
{
    if (! monitoring)
        return;

    backend.closeAllStreams();
    monitoring = false;
}

void CaptureCoordinator::stopMirroring()
{
    if (pipeline != nullptr)
        pipeline->stopMirroring();
}

bool CaptureCoordinator::startRecording (const std::string& sessionFolder, int bitDepth,
                                         const std::string& originTimestamp,
                                         const std::string& mirrorFolder)
{
    if (channels.empty() || isRecording())
        return false;

    std::vector<WriteChannelSpec> specs;
    specs.reserve (channels.size());

    for (const auto& ch : channels)
        specs.push_back ({ ch.fileName, ch.trimDb });

    auto p = std::make_unique<WritePipeline>();

    if (! p->start (sessionFolder, specs, sampleRate, bitDepth, originTimestamp, mirrorFolder))
        return false;

    // Published only once fully started, so the audio thread never sees a
    // half-built pipeline. The release store pairs with the acquire load in
    // mixAndPublish: a callback that sees the pointer also sees every write
    // start() made to the object behind it.
    pipeline = std::move (p);
    activePipeline.store (pipeline.get(), std::memory_order_release);
    return true;
}

void CaptureCoordinator::stopRecording()
{
    if (pipeline == nullptr)
        return;

    // Retire the pointer first, then wait for any callback already past the
    // load to finish with it. Without this the audio thread could be inside
    // pushBlock while stop() joined the writer thread and freed the ring out
    // from under it -- a use-after-free that would present as an intermittent
    // crash on stopping a take, which is the worst possible moment for one.
    activePipeline.store (nullptr, std::memory_order_release);

    while (pipelineUsers.load (std::memory_order_acquire) != 0)
        std::this_thread::yield();

    auto p = std::move (pipeline);
    p->stop();
}

void CaptureCoordinator::setChannelLive (const std::string& deviceId, bool live)
{
    for (size_t i = 0; i < channels.size(); ++i)
    {
        if (channels[i].deviceId != deviceId)
            continue;

        if (pipeline != nullptr)
            pipeline->setChannelLive (static_cast<int> (i), live);

        if (i < deviceStreams.size())
            deviceStreams[i]->setLive (live);

        return;
    }
}

void CaptureCoordinator::setChannelTrimDb (int index, float trimDb) noexcept
{
    if (index < 0 || index >= static_cast<int> (channels.size())
        || index >= static_cast<int> (trimGains.size()))
        return;

    channels[static_cast<size_t> (index)].trimDb = trimDb;

    // One aligned float store, so the callback either sees the old gain or the
    // new one -- never a torn value and never a resized vector (§11).
    trimGains[static_cast<size_t> (index)] = MonitorBus::trimDbToLinearGain (trimDb);

    if (pipeline != nullptr)
        pipeline->setChannelTrimDb (index, trimDb);
}

float CaptureCoordinator::getChannelTrimDb (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (channels.size()))
        return 0.0f;

    return channels[static_cast<size_t> (index)].trimDb;
}

Metering* CaptureCoordinator::getChannelMetering (int index) noexcept
{
    if (index < 0 || index >= static_cast<int> (channelMeters.size()))
        return nullptr;

    return channelMeters[static_cast<size_t> (index)].get();
}

void CaptureCoordinator::pushDeviceBlock (int deviceIndex, const float* samples, int numSamples) noexcept
{
    if (deviceIndex < 0 || deviceIndex >= static_cast<int> (deviceStreams.size()))
        return;

    // §3.2: straight into this device's own ring. The output clock decides when
    // it is consumed, and at what rate.
    deviceStreams[static_cast<size_t> (deviceIndex)]->pushBlock (samples, numSamples);
}

void CaptureCoordinator::pushDeviceBlockMultiChannel (int deviceIndex, const float* const* inputs,
                                                      int numInputs, int numSamples) noexcept
{
    if (deviceIndex < 0 || deviceIndex >= static_cast<int> (channelLayouts.size())
        || inputs == nullptr || numInputs < 2 || numSamples <= 0)
        return;

    auto& layout = *channelLayouts[static_cast<size_t> (deviceIndex)];

    const float* left = inputs[0];
    const float* right = inputs[1];

    if (left == nullptr || right == nullptr)
        return pushDeviceBlock (deviceIndex, inputs[0], numSamples);

    // §11: two passes over the block, no allocation, no locking. §2.1 wants a
    // peak per channel, their correlation, and the difference in their RMS.
    float peakLeft = 0.0f, peakRight = 0.0f;
    double sumLL = 0.0, sumRR = 0.0, sumLR = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const float l = left[i];
        const float r = right[i];

        peakLeft = std::max (peakLeft, std::abs (l));
        peakRight = std::max (peakRight, std::abs (r));

        sumLL += static_cast<double> (l) * l;
        sumRR += static_cast<double> (r) * r;
        sumLR += static_cast<double> (l) * r;
    }

    const auto toDb = [] (float linear)
    {
        // A floor rather than -inf, so the analyzer's comparisons stay ordered
        // and a digitally silent channel is simply very quiet.
        return linear > 1.0e-10f ? 20.0f * std::log10 (linear) : -200.0f;
    };

    const double rmsLeft = std::sqrt (sumLL / static_cast<double> (numSamples));
    const double rmsRight = std::sqrt (sumRR / static_cast<double> (numSamples));

    // Pearson correlation over the block. Zero when either side is silent,
    // which is the honest answer: nothing to be correlated with.
    const double denominator = std::sqrt (sumLL) * std::sqrt (sumRR);
    const float correlation = denominator > 1.0e-12
                                  ? static_cast<float> (sumLR / denominator)
                                  : 0.0f;

    const float rmsDiffDb = std::abs (toDb (static_cast<float> (rmsLeft))
                                      - toDb (static_cast<float> (rmsRight)));

    const double blockSeconds = sampleRate > 0.0
                                    ? static_cast<double> (numSamples) / sampleRate
                                    : 0.0;

    layout.analyzer.processBlock (toDb (peakLeft), toDb (peakRight),
                                  correlation, rmsDiffDb, blockSeconds);

    // The side is free to move only until §2.1 has decided, and never once a
    // take is running: swapping which channel feeds a stem mid-file would put a
    // discontinuity in the middle of the recording, which is a worse failure
    // than the one this fixes (§6.5 fixes the take's shape for its duration).
    if (! layout.frozen)
    {
        // Checked before the store, not after. Reversing these leaves a
        // one-block window in which a take that has just started still adopts a
        // new side -- which is exactly the discontinuity the freeze exists to
        // prevent, made rarer and therefore harder to find.
        if (activePipeline.load (std::memory_order_acquire) != nullptr)
        {
            layout.frozen = true;
        }
        else
        {
            layout.source.store (layout.analyzer.getMonoSourceChannel(), std::memory_order_relaxed);
            layout.decision.store (static_cast<int> (layout.analyzer.getDecision()),
                                   std::memory_order_relaxed);

            if (layout.analyzer.getDecision() != ChannelLayoutDecision::Pending)
                layout.frozen = true;
        }
    }

    const int side = layout.source.load (std::memory_order_relaxed);

    // By pointer: collapsing to mono is choosing which channel to read, not
    // copying one.
    pushDeviceBlock (deviceIndex, side == 1 ? right : left, numSamples);
}

int CaptureCoordinator::getChannelLayoutSource (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (channelLayouts.size()))
        return -1;

    return channelLayouts[static_cast<size_t> (index)]->source.load (std::memory_order_relaxed);
}

ChannelLayoutDecision CaptureCoordinator::getChannelLayoutDecision (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (channelLayouts.size()))
        return ChannelLayoutDecision::Pending;

    return static_cast<ChannelLayoutDecision> (
        channelLayouts[static_cast<size_t> (index)]->decision.load (std::memory_order_relaxed));
}

void CaptureCoordinator::processOutputBlock (float* const* outputs, int numOutputs, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    const auto callbackStart = std::chrono::steady_clock::now();

    const int channelCount = static_cast<int> (deviceStreams.size());
    const size_t block = static_cast<size_t> (numSamples);

    // Sized at startMonitoring(); never grow here (§11).
    if (deviceScratch.size() < static_cast<size_t> (channelCount) * block
        || devicePointers.size() < static_cast<size_t> (channelCount))
        return noteCallbackLoad (callbackStart, numSamples);

    // §3.2: every device is pulled onto this callback's timebase here. This is
    // the single point where the independent USB clocks become one aligned
    // frame, and every channel is corrected onto it -- including the one §3.1
    // names as clock master, whose crystal is no more this clock than any
    // other mic's is.
    for (int ch = 0; ch < channelCount; ++ch)
    {
        float* destination = deviceScratch.data() + static_cast<size_t> (ch) * block;
        deviceStreams[static_cast<size_t> (ch)]->pull (destination, numSamples);
        devicePointers[static_cast<size_t> (ch)] = destination;
    }

    mixAndPublish (devicePointers.data(), channelCount, outputs, numOutputs, numSamples);
    noteCallbackLoad (callbackStart, numSamples);
}

void CaptureCoordinator::processAudioBlock (const float* const* inputs, int numInputs,
                                            float* const* outputs, int numOutputs,
                                            int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    // A clock read, not a syscall, on both shipping platforms -- no allocation,
    // no lock, nothing §11 forbids.
    const auto callbackStart = std::chrono::steady_clock::now();

    // Aggregate path: the OS has already aligned these channels, so they go
    // straight to the mixer without passing through the per-device rings.
    const int channelCount = std::min (numInputs, static_cast<int> (channels.size()));

    mixAndPublish (inputs, channelCount, outputs, numOutputs, numSamples);
    noteCallbackLoad (callbackStart, numSamples);
}

void CaptureCoordinator::mixAndPublish (const float* const* inputs, int channelCount,
                                        float* const* outputs, int numOutputs,
                                        int numSamples) noexcept
{
    if (inputs == nullptr || channelCount <= 0)
        return;

    // §6: recording gets the raw channels. §4 keeps stems at unity and applies
    // trim only to the mix, which the pipeline does itself.
    //
    // The busy count is taken before the load and released after the call, so
    // stopRecording cannot free the pipeline between the two.
    pipelineUsers.fetch_add (1, std::memory_order_acquire);

    if (auto* activeWriter = activePipeline.load (std::memory_order_acquire))
        if (channelCount == static_cast<int> (channels.size()))
            activeWriter->pushBlock (inputs, channelCount, numSamples);

    pipelineUsers.fetch_sub (1, std::memory_order_release);

    // §8.2: the audio thread does only max-abs per block into the meter.
    for (int ch = 0; ch < channelCount; ++ch)
        if (inputs[ch] != nullptr && ch < static_cast<int> (channelMeters.size()))
            channelMeters[static_cast<size_t> (ch)]->processAudioBlock (inputs[ch], numSamples);

    if (outputs == nullptr || numOutputs <= 0)
        return;

    // §5.1: one mix, containing every microphone including the listener's own,
    // summed at unity with no attenuation for channel count.
    if (mixScratch.size() < static_cast<size_t> (numSamples))
        return; // sized at startMonitoring(); never grow here (§11)

    if (static_cast<int> (trimFrame.size()) < channelCount
        || static_cast<int> (trimGains.size()) < channelCount)
        return; // sized at startMonitoring(); never grow here (§11)

    for (int s = 0; s < numSamples; ++s)
    {
        // §4: trim is applied to the mix only. The stems the pipeline already
        // received above stay at unity, which is what makes trim a monitoring
        // decision the recording cannot be damaged by.
        for (int ch = 0; ch < channelCount; ++ch)
            trimFrame[static_cast<size_t> (ch)] =
                (inputs[ch] != nullptr) ? inputs[ch][s] * trimGains[static_cast<size_t> (ch)] : 0.0f;

        // §5.1: master volume is an output-stage gain, deliberately outside
        // processSample, so the bus keeps its -3 dBFS ceiling regardless of how
        // loud the listener happens to be running their headphones.
        mixScratch[static_cast<size_t> (s)] =
            monitorBus.applyMasterVolume (monitorBus.processSample (trimFrame));
    }

    mixMeter.processAudioBlock (mixScratch.data(), numSamples);

    // §5.2: the same mix to every output channel -- no per-listener variation.
    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            std::copy (mixScratch.begin(), mixScratch.begin() + numSamples, outputs[ch]);
}

void CaptureCoordinator::setMasterChannel (int index) noexcept
{
    // §3.1: exactly one reference. Nothing about the capture path changes here
    // -- every channel is corrected onto the output clock either way (§3.2) --
    // so this only moves which channel §3.3's drift figures are measured from,
    // and which device Application publishes as the aggregate's clock source.
    // That makes it safe to call mid-take, unlike a change to the channel set.
    masterChannel = (index >= 0 && index < static_cast<int> (deviceStreams.size())) ? index : -1;
}

double CaptureCoordinator::getMasterDriftPpm() const noexcept
{
    if (masterChannel < 0 || masterChannel >= static_cast<int> (deviceStreams.size()))
        return 0.0;

    return deviceStreams[static_cast<size_t> (masterChannel)]->getDriftPpm();
}

void CaptureCoordinator::tickDriftReporting (double elapsedSeconds) noexcept
{
    const double reference = getMasterDriftPpm();

    for (auto& stream : deviceStreams)
        stream->tickDriftReporting (elapsedSeconds, reference);
}

double CaptureCoordinator::getChannelDriftPpm (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
        return 0.0;

    // §3.3: "positive means this device runs fast relative to the master". Each
    // stream's own figure is its correction against the output clock, and that
    // clock's skew is common to all of them, so the difference is exactly the
    // device-against-master number §3.3 asks for -- and the master reports zero
    // against itself by construction.
    return deviceStreams[static_cast<size_t> (index)]->getDriftPpm() - getMasterDriftPpm();
}

bool CaptureCoordinator::hasSustainedExcessDrift (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
        return false;

    return deviceStreams[static_cast<size_t> (index)]->hasSustainedExcessDrift();
}

uint64_t CaptureCoordinator::getUnderrunSamples (int index) const noexcept
{
    if (index < 0 || index >= static_cast<int> (deviceStreams.size()))
        return 0;

    return deviceStreams[static_cast<size_t> (index)]->getUnderrunSamples();
}

void CaptureCoordinator::noteCallbackLoad (std::chrono::steady_clock::time_point start,
                                           int numSamples) noexcept
{
    if (sampleRate <= 0.0)
        return;

    const auto elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    const auto available = static_cast<double> (numSamples) / sampleRate;

    if (available <= 0.0)
        return;

    // Smoothed a little: a single long callback is normal, a sustained high
    // average is what §6.6 warns about.
    const auto instant = elapsed / available;
    const auto previous = callbackLoad.load (std::memory_order_relaxed);

    callbackLoad.store (previous + 0.05 * (instant - previous), std::memory_order_relaxed);
}

} // namespace mma
