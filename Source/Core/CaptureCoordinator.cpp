#include "CaptureCoordinator.h"
#include <algorithm>
#include <cmath>

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

    auto callback = [this] (const float* const* inputs, int numInputs,
                            float* const* outputs, int numOutputs, int numSamples)
    {
        processAudioBlock (inputs, numInputs, outputs, numOutputs, numSamples);
    };

    // §5.2: exactly one output stream, ever.
    if (! outputDeviceId.empty()
        && ! backend.openExclusiveOutputStream (outputDeviceId, sampleRate, bufferSize, callback))
    {
        monitorProblem = "Couldn't open your headphones for low-latency playback.";
        backend.closeAllStreams();
        return false;
    }

    for (const auto& ch : channels)
    {
        if (! backend.openInputStream (ch.deviceId, sampleRate, bufferSize, callback))
        {
            monitorProblem = ch.displayName + " couldn't be opened for recording.";
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

bool CaptureCoordinator::startRecording (const std::string& sessionFolder, int bitDepth,
                                         const std::string& originTimestamp)
{
    if (channels.empty() || isRecording())
        return false;

    std::vector<WriteChannelSpec> specs;
    specs.reserve (channels.size());

    for (const auto& ch : channels)
        specs.push_back ({ ch.fileName, ch.trimDb });

    auto p = std::make_unique<WritePipeline>();

    if (! p->start (sessionFolder, specs, sampleRate, bitDepth, originTimestamp))
        return false;

    // Published only once fully started, so the audio thread never sees a
    // half-built pipeline.
    pipeline = std::move (p);
    return true;
}

void CaptureCoordinator::stopRecording()
{
    if (pipeline == nullptr)
        return;

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

        return;
    }
}

Metering* CaptureCoordinator::getChannelMetering (int index) noexcept
{
    if (index < 0 || index >= static_cast<int> (channelMeters.size()))
        return nullptr;

    return channelMeters[static_cast<size_t> (index)].get();
}

void CaptureCoordinator::processAudioBlock (const float* const* inputs, int numInputs,
                                            float* const* outputs, int numOutputs,
                                            int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    const int channelCount = std::min (numInputs, static_cast<int> (channels.size()));

    // §6: recording gets the raw inputs. §4 keeps stems at unity and applies
    // trim only to the mix, which the pipeline does itself.
    if (pipeline != nullptr && channelCount == static_cast<int> (channels.size()))
        pipeline->pushBlock (inputs, channelCount, numSamples);

    // §8.2: the audio thread does only max-abs per block into the meter.
    for (int ch = 0; ch < channelCount; ++ch)
        if (inputs[ch] != nullptr && ch < static_cast<int> (channelMeters.size()))
            channelMeters[static_cast<size_t> (ch)]->processAudioBlock (inputs[ch], numSamples);

    if (outputs == nullptr || numOutputs <= 0)
        return;

    // §5.1: one mix, containing every microphone including the listener's own,
    // summed at unity with no attenuation for channel count.
    const size_t needed = static_cast<size_t> (numSamples);

    if (mixScratch.size() < needed)
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

} // namespace mma
