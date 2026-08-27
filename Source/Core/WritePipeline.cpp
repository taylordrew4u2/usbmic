#include "WritePipeline.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace mma {

namespace {

float dbToGain (float db) noexcept
{
    return std::pow (10.0f, db / 20.0f);
}

/// How long the writer sleeps when the ring is empty. Short enough that stopping
/// is not perceptibly slow, long enough not to spin a core.
constexpr int kWriterIdleMs = 5;

} // namespace

WritePipeline::~WritePipeline()
{
    stop();
}

bool WritePipeline::start (const std::string& sessionFolder,
                           const std::vector<WriteChannelSpec>& channels,
                           double rate, int bitDepth,
                           const std::string& originTimestamp)
{
    if (running.load (std::memory_order_acquire) || channels.empty())
        return false;

    numChannels = static_cast<int> (channels.size());
    sampleRate = rate;

    // §6.3: 30 seconds at the current channel count and rate, minimum 64 MB.
    ring.reset (RingBuffer::minimumCapacitySamples (rate, numChannels));

    stemWriters.clear();
    trimGains.clear();

    for (const auto& spec : channels)
    {
        auto writer = std::make_unique<SessionWriter>();

        // §6.1: stems are mono, one file per microphone, at unity gain.
        if (! writer->open (sessionFolder + "/" + spec.fileName, rate, 1, bitDepth, originTimestamp))
            return false;

        stemWriters.push_back (std::move (writer));
        trimGains.push_back (dbToGain (spec.trimDb));
    }

    mixWriter = std::make_unique<SessionWriter>();
    if (! mixWriter->open (sessionFolder + "/MIX", rate, 1, bitDepth, originTimestamp))
        return false;

    // std::atomic is not copyable, so the vector is built in place.
    std::vector<std::atomic<bool>> live (static_cast<size_t> (numChannels));
    for (auto& l : live)
        l.store (true, std::memory_order_relaxed);
    channelLive = std::move (live);

    // Writer-thread scratch, sized once here so the drain loop never allocates.
    const size_t drainFrames = 4096;
    drainBuffer.assign (drainFrames * static_cast<size_t> (numChannels), 0.0f);
    stemScratch.assign (drainFrames, 0.0f);
    mixScratch.assign (drainFrames, 0.0f);

    framesAccepted.store (0, std::memory_order_relaxed);
    framesDropped.store (0, std::memory_order_relaxed);

    running.store (true, std::memory_order_release);
    writerThread = std::thread ([this] { runWriterThread(); });
    return true;
}

bool WritePipeline::pushBlock (const float* const* channelData, int numChannels_, int numSamples) noexcept
{
    if (! running.load (std::memory_order_acquire) || numChannels_ != numChannels || numSamples <= 0)
        return false;

    // Interleave straight into the ring. Writing per-frame keeps the audio
    // thread free of any temporary buffer, which §11 would not allow it to
    // allocate anyway.
    const size_t needed = static_cast<size_t> (numSamples) * static_cast<size_t> (numChannels);

    if (ring.availableForWrite() < needed)
    {
        // §0.1: a dropped sample is the one unacceptable failure, so this is
        // counted rather than quietly ignored.
        framesDropped.fetch_add (static_cast<uint64_t> (numSamples), std::memory_order_relaxed);
        return false;
    }

    for (int f = 0; f < numSamples; ++f)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            // §6.5: an unplugged mic writes silence into its existing channel
            // rather than the file layout changing mid-recording.
            const bool live = channelLive[static_cast<size_t> (ch)].load (std::memory_order_relaxed);
            const float sample = (live && channelData[ch] != nullptr) ? channelData[ch][f] : 0.0f;
            ring.write (&sample, 1);
        }
    }

    framesAccepted.fetch_add (static_cast<uint64_t> (numSamples), std::memory_order_relaxed);
    return true;
}

void WritePipeline::setChannelLive (int channelIndex, bool live) noexcept
{
    if (channelIndex >= 0 && channelIndex < static_cast<int> (channelLive.size()))
        channelLive[static_cast<size_t> (channelIndex)].store (live, std::memory_order_relaxed);
}

void WritePipeline::drainOnce (bool finalFlush)
{
    const size_t framesCapacity = stemScratch.size();

    for (;;)
    {
        const size_t availableSamples = ring.availableForRead();
        const size_t availableFrames = availableSamples / static_cast<size_t> (numChannels);

        if (availableFrames == 0)
            return;

        const size_t frames = std::min (availableFrames, framesCapacity);
        const size_t samples = frames * static_cast<size_t> (numChannels);

        if (ring.read (drainBuffer.data(), samples) != samples)
            return;

        std::fill (mixScratch.begin(), mixScratch.begin() + static_cast<long> (frames), 0.0f);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float gain = trimGains[static_cast<size_t> (ch)];

            for (size_t f = 0; f < frames; ++f)
            {
                const float sample = drainBuffer[f * static_cast<size_t> (numChannels) + static_cast<size_t> (ch)];

                // §4: the stem is unity gain, always. A bad trim decision must
                // not be baked into the raw material.
                stemScratch[f] = sample;

                // §5.1: unity summing, no attenuation with channel count. Trim
                // applies here and only here.
                mixScratch[f] += sample * gain;
            }

            stemWriters[static_cast<size_t> (ch)]->writeInterleaved (stemScratch.data(), frames);
        }

        // §6.1: the mix file gets its own limiter instance at -1 dBFS, separate
        // from the monitor limiter, so a monitor mute never silences the file.
        for (size_t f = 0; f < frames; ++f)
            mixScratch[f] = MixBusLimiter::processSample (mixScratch[f]);

        mixWriter->writeInterleaved (mixScratch.data(), frames);

        const double elapsed = static_cast<double> (frames) / sampleRate;
        for (auto& w : stemWriters)
            w->tick (elapsed);
        mixWriter->tick (elapsed);

        if (! finalFlush && frames < framesCapacity)
            return;
    }
}

void WritePipeline::runWriterThread()
{
    while (running.load (std::memory_order_acquire))
    {
        drainOnce (false);
        std::this_thread::sleep_for (std::chrono::milliseconds (kWriterIdleMs));
    }
}

void WritePipeline::stop()
{
    if (! running.exchange (false, std::memory_order_acq_rel))
        return;

    if (writerThread.joinable())
        writerThread.join();

    // Everything still in the ring belongs to the take. §0.1: audio is never
    // lost, including the last block before stop.
    drainOnce (true);

    for (auto& w : stemWriters)
        w->close();
    stemWriters.clear();

    if (mixWriter != nullptr)
    {
        mixWriter->close();
        mixWriter.reset();
    }
}

} // namespace mma
