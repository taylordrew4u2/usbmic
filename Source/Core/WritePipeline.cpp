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
                           const std::string& originTimestamp,
                           const std::string& mirrorFolder)
{
    // A fresh take starts with a clean slate: a failure from a previous one
    // must never make this one look doomed before it has written a byte.
    cardWriteFailed.store (false, std::memory_order_release);
    mirrorWriteFailed.store (false, std::memory_order_release);

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
        {
            // Close what did open. A take that failed to start must not leave
            // half-written headers on the card for the recovery pass to find.
            for (auto& opened : stemWriters)
                opened->close();

            stemWriters.clear();
            return false;
        }

        stemWriters.push_back (std::move (writer));
        trimGains.push_back (dbToGain (spec.trimDb));
    }

    mixWriter = std::make_unique<SessionWriter>();

    if (! mixWriter->open (sessionFolder + "/MIX", rate, 1, bitDepth, originTimestamp))
    {
        for (auto& opened : stemWriters)
            opened->close();

        stemWriters.clear();
        mixWriter.reset();
        return false;
    }

    // §6.3: the mirror is a safety net, so failing to open it degrades to
    // card-only rather than failing a take the user is waiting to start.
    mirrorStemWriters.clear();
    mirrorMixWriter.reset();
    mirroring.store (false, std::memory_order_release);

    if (! mirrorFolder.empty() && openMirrorWriters (mirrorFolder, channels, rate, bitDepth, originTimestamp))
        mirroring.store (true, std::memory_order_release);

    // std::atomic is not copyable, so the vector is built in place.
    std::vector<std::atomic<bool>> live (static_cast<size_t> (numChannels));
    for (auto& l : live)
        l.store (true, std::memory_order_relaxed);
    channelLive = std::move (live);

    // Audio-thread scratch, sized once here so pushBlock never allocates (§11).
    interleaveScratch.assign (kInterleaveChunkFrames * static_cast<size_t> (numChannels), 0.0f);

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

    // Interleave in chunks. The whole block is known to fit -- that was checked
    // above, and this is the ring's only producer -- so every chunk lands whole.
    //
    // The per-sample ring.write this replaces cost a modulo and an atomic
    // release-store for every sample of every channel: at 8 mics and 48 kHz that
    // is 384,000 release-stores a second, on the one thread that must never miss
    // its deadline.
    int frameOffset = 0;

    while (frameOffset < numSamples)
    {
        const int chunkFrames = std::min (static_cast<int> (kInterleaveChunkFrames),
                                          numSamples - frameOffset);
        float* destination = interleaveScratch.data();

        for (int f = 0; f < chunkFrames; ++f)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                // §6.5: an unplugged mic writes silence into its existing channel
                // rather than the file layout changing mid-recording.
                const bool live = channelLive[static_cast<size_t> (ch)].load (std::memory_order_relaxed);
                *destination++ = (live && channelData[ch] != nullptr)
                                     ? channelData[ch][frameOffset + f]
                                     : 0.0f;
            }
        }

        ring.write (interleaveScratch.data(),
                    static_cast<size_t> (chunkFrames) * static_cast<size_t> (numChannels));

        frameOffset += chunkFrames;
    }

    framesAccepted.fetch_add (static_cast<uint64_t> (numSamples), std::memory_order_relaxed);
    return true;
}

void WritePipeline::setChannelTrimDb (int channelIndex, float trimDb) noexcept
{
    if (channelIndex < 0 || channelIndex >= static_cast<int> (trimGains.size()))
        return;

    // Single float store into an already-sized vector: the writer thread reads
    // either the old gain or the new one, never a torn or reallocated value.
    trimGains[static_cast<size_t> (channelIndex)] = dbToGain (trimDb);
}

void WritePipeline::setChannelLive (int channelIndex, bool live) noexcept
{
    if (channelIndex >= 0 && channelIndex < static_cast<int> (channelLive.size()))
        channelLive[static_cast<size_t> (channelIndex)].store (live, std::memory_order_relaxed);
}

bool WritePipeline::openMirrorWriters (const std::string& mirrorFolder,
                                       const std::vector<WriteChannelSpec>& channels,
                                       double rate, int bitDepth,
                                       const std::string& originTimestamp)
{
    for (const auto& spec : channels)
    {
        auto writer = std::make_unique<SessionWriter>();

        if (! writer->open (mirrorFolder + "/" + spec.fileName, rate, 1, bitDepth, originTimestamp))
        {
            mirrorStemWriters.clear();
            return false;
        }

        mirrorStemWriters.push_back (std::move (writer));
    }

    auto mix = std::make_unique<SessionWriter>();

    if (! mix->open (mirrorFolder + "/MIX", rate, 1, bitDepth, originTimestamp))
    {
        mirrorStemWriters.clear();
        return false;
    }

    mirrorMixWriter = std::move (mix);
    return true;
}

void WritePipeline::stopMirroring()
{
    // Only flips the flag. The writer thread owns the files, so it closes them
    // on its next pass rather than this being a cross-thread close.
    mirroring.store (false, std::memory_order_release);
}

void WritePipeline::drainOnce (bool finalFlush)
{
    const size_t framesCapacity = stemScratch.size();

    // Read once per pass: if the mirror is stopped mid-drain, this pass still
    // completes coherently and the next one closes the files.
    const bool mirrorActiveForThisPass = mirroring.load (std::memory_order_acquire);

    if (! mirrorActiveForThisPass && ! mirrorStemWriters.empty())
    {
        // §6.3: stopped for space, or stopped by the user. Close the partial
        // copy properly so its headers are valid rather than truncated.
        for (auto& w : mirrorStemWriters)
            w->close();

        mirrorStemWriters.clear();

        if (mirrorMixWriter != nullptr)
        {
            mirrorMixWriter->close();
            mirrorMixWriter.reset();
        }
    }

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

            // §6.5: the return value is the card telling us it has gone. It was
            // discarded here, which is why pulling a card mid-take used to look
            // exactly like a healthy recording.
            if (! stemWriters[static_cast<size_t> (ch)]->writeInterleaved (stemScratch.data(), frames))
                cardWriteFailed.store (true, std::memory_order_release);

            // §6.3: same frames, second destination. Written from the same
            // scratch so the copy cannot diverge from the original.
            if (mirrorActiveForThisPass && ch < static_cast<int> (mirrorStemWriters.size()))
                if (! mirrorStemWriters[static_cast<size_t> (ch)]->writeInterleaved (stemScratch.data(), frames))
                    mirrorWriteFailed.store (true, std::memory_order_release);
        }

        // §6.1: the mix file gets its own limiter instance at -1 dBFS, separate
        // from the monitor limiter, so a monitor mute never silences the file.
        for (size_t f = 0; f < frames; ++f)
            mixScratch[f] = MixBusLimiter::processSample (mixScratch[f]);

        if (! mixWriter->writeInterleaved (mixScratch.data(), frames))
            cardWriteFailed.store (true, std::memory_order_release);

        if (mirrorActiveForThisPass && mirrorMixWriter != nullptr)
            if (! mirrorMixWriter->writeInterleaved (mixScratch.data(), frames))
                mirrorWriteFailed.store (true, std::memory_order_release);

        const double elapsed = static_cast<double> (frames) / sampleRate;
        for (auto& w : stemWriters)
            w->tick (elapsed);
        mixWriter->tick (elapsed);

        if (mirrorActiveForThisPass)
        {
            for (auto& w : mirrorStemWriters)
                w->tick (elapsed);

            if (mirrorMixWriter != nullptr)
                mirrorMixWriter->tick (elapsed);
        }

        // §6.3: a mirror that has failed stops for the rest of the take, the
        // same way one that ran out of room does. The card write is untouched:
        // the mirror must never take the recording down with it.
        if (mirrorWriteFailed.load (std::memory_order_acquire) && mirroring.load (std::memory_order_acquire))
            stopMirroring();

        // §6.5: the destination is gone. Draining further would loop writing
        // into a dead handle; the owner stops and finalizes the take.
        if (cardWriteFailed.load (std::memory_order_acquire))
            return;

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

    // §6.3: the mirror is closed last and the same way, so a take that ended
    // normally leaves two complete, independently valid copies.
    for (auto& w : mirrorStemWriters)
        w->close();
    mirrorStemWriters.clear();

    if (mirrorMixWriter != nullptr)
    {
        mirrorMixWriter->close();
        mirrorMixWriter.reset();
    }

    mirroring.store (false, std::memory_order_release);
}

} // namespace mma
