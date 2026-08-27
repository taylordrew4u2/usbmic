#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "RingBuffer.h"
#include "SessionWriter.h"
#include "MixBusLimiter.h"

namespace mma {

struct WriteChannelSpec
{
    std::string fileName; // "01_Yeti-Kitchen", already sanitized per §6.2
    float trimDb = 0.0f;  // §4: affects the mix file only, never the stem
};

/// §6.3 write pipeline: the audio callback hands blocks to a lock-free ring
/// buffer and a dedicated writer thread drains it to disk. §11 forbids
/// allocation, locking, logging and file I/O in the callback, so everything the
/// audio thread touches here is sized up front and lock-free.
class WritePipeline
{
public:
    ~WritePipeline();

    /// Opens one stem per channel plus MIX, all sharing a session origin so a
    /// DAW aligns them on import (§6.1).
    bool start (const std::string& sessionFolder,
                const std::vector<WriteChannelSpec>& channels,
                double sampleRate, int bitDepth,
                const std::string& originTimestamp);

    /// Audio-thread entry point. Real-time safe: no allocation, no locking, no
    /// file I/O. Returns false when the ring buffer could not take the whole
    /// block, which is a dropout and must be reported (§0.1, §6.5).
    bool pushBlock (const float* const* channelData, int numChannels, int numSamples) noexcept;

    /// §6.5: a channel whose mic is unplugged writes silence rather than
    /// changing the file layout mid-recording.
    void setChannelLive (int channelIndex, bool live) noexcept;

    void stop();

    bool isRunning() const noexcept { return running.load (std::memory_order_acquire); }

    /// §6.5: warn at 50% fill, degrade at 90%.
    double getFillFraction() const noexcept { return ring.fillFraction(); }

    /// Total frames accepted from the audio thread, for session.json.
    uint64_t getFramesAccepted() const noexcept { return framesAccepted.load (std::memory_order_relaxed); }

    /// Frames the ring buffer could not accept. Any value above zero means
    /// audio was lost, which §0.1 treats as the one unacceptable failure.
    uint64_t getFramesDropped() const noexcept { return framesDropped.load (std::memory_order_relaxed); }

private:
    // Constructed small and resized by start(), which is the only place the
    // real channel count and rate are known. RingBuffer::reset reallocates, so
    // it must never be called while the audio thread is running.
    RingBuffer ring { 1 };
    std::vector<std::unique_ptr<SessionWriter>> stemWriters;
    std::unique_ptr<SessionWriter> mixWriter;
    MixBusLimiter mixLimiter;

    std::vector<float> trimGains;      // linear, from WriteChannelSpec::trimDb
    std::vector<std::atomic<bool>> channelLive;

    std::thread writerThread;
    std::atomic<bool> running { false };
    std::atomic<uint64_t> framesAccepted { 0 };
    std::atomic<uint64_t> framesDropped { 0 };

    int numChannels = 0;
    double sampleRate = 48000.0;

    // Writer-thread scratch, allocated at start() so the drain loop does not.
    std::vector<float> drainBuffer;
    std::vector<float> stemScratch;
    std::vector<float> mixScratch;

    void runWriterThread();
    void drainOnce (bool finalFlush);
};

} // namespace mma
