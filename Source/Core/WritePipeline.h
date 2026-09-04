#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "RingBuffer.h"
#include "SessionWriter.h"
#include "MixBusLimiter.h"
#include "LoudnessMeter.h"

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
    /// mirrorFolder is §6.3's redundant local copy. Empty means no mirror.
    /// A mirror that cannot be opened is not an error: §6.3 makes the mirror a
    /// safety net, so losing it degrades to card-only rather than failing the
    /// take.
    bool start (const std::string& sessionFolder,
                const std::vector<WriteChannelSpec>& channels,
                double sampleRate, int bitDepth,
                const std::string& originTimestamp,
                const std::string& mirrorFolder = {});

    /// §6.3: the mirror stops when the internal drive runs low, and never
    /// restarts within the same take -- a copy with a hole in it is not a copy.
    void stopMirroring();
    bool isMirroring() const noexcept { return mirroring.load (std::memory_order_acquire); }

    /// Audio-thread entry point. Real-time safe: no allocation, no locking, no
    /// file I/O. Returns false when the ring buffer could not take the whole
    /// block, which is a dropout and must be reported (§0.1, §6.5).
    bool pushBlock (const float* const* channelData, int numChannels, int numSamples) noexcept;

    /// §6.5: at 90% ring fill with no mirror to fall back on, the stems stop
    /// and the mix keeps going. A complete mix is worth more than eight stems
    /// that all have the same hole in them, and the ring draining at a fraction
    /// of the byte rate is what stops the hole growing.
    ///
    /// Never restarts within a take, for the same reason the mirror does not
    /// (§6.3): stems that resume mid-file are worse than stems that stopped,
    /// because the gap is invisible in the waveform.
    void fallBackToMixOnly() noexcept;
    bool isMixOnly() const noexcept { return mixOnly.load (std::memory_order_acquire); }

    /// §6.5: a channel whose mic is unplugged writes silence rather than
    /// changing the file layout mid-recording.
    void setChannelLive (int channelIndex, bool live) noexcept;

    /// §4: trim moves the mix file, never the stems. Live, because the user can
    /// turn a mic down mid-take and the mix should follow.
    void setChannelTrimDb (int channelIndex, float trimDb) noexcept;

    void stop();

    bool isRunning() const noexcept { return running.load (std::memory_order_acquire); }

    /// BS.1770 loudness of the mix as written, measured on the writer thread.
    ///
    /// Measured here rather than in the audio callback because §11 forbids this
    /// much arithmetic per block there, and because this is the one place the
    /// mix exists exactly as it lands on disk -- summed, trimmed and through
    /// the limiter. A figure taken anywhere earlier would describe a mix nobody
    /// receives.
    double getIntegratedLufs() const;
    double getTruePeakDbtp() const;
    int getLoudnessBlockCount() const;

    /// §6.5: warn at 50% fill, degrade at 90%.
    double getFillFraction() const noexcept { return ring.fillFraction(); }

    /// Total frames accepted from the audio thread, for session.json.
    uint64_t getFramesAccepted() const noexcept { return framesAccepted.load (std::memory_order_relaxed); }

    /// Frames the ring buffer could not accept. Any value above zero means
    /// audio was lost, which §0.1 treats as the one unacceptable failure.
    uint64_t getFramesDropped() const noexcept { return framesDropped.load (std::memory_order_relaxed); }

    /// The largest absolute sample value written since start(), across every
    /// channel. Zero means this take wrote nothing but silence -- which size
    /// alone cannot tell you, because a dead stream still fills the files.
    float getPeakWritten() const noexcept { return peakWritten.load (std::memory_order_relaxed); }

    /// §6.5 "target card removed": true once a write to the destination itself
    /// has failed, which is what pulling the card mid-take looks like from here.
    ///
    /// SessionWriter has always returned false on an unrecoverable write and its
    /// own header says the caller handles it per §6.5. This pipeline was that
    /// caller and discarded every one of those returns, so a pulled card wrote
    /// into the void: no stop, no finalize, no alert, and the elapsed time still
    /// climbing. Once this is true the pipeline stops writing rather than
    /// looping on a dead handle, and the owner must stop and finalize the take.
    bool hasCardWriteFailed() const noexcept { return cardWriteFailed.load (std::memory_order_acquire); }

    /// §6.3: the mirror failing is survivable and must never stop the card --
    /// the mirror exists to turn a card failure into an inconvenience, so it
    /// cannot be allowed to become one itself. A mirror write that fails stops
    /// the mirror for the rest of the take, exactly as running out of internal
    /// space does, and the card write continues untouched.
    bool hasMirrorWriteFailed() const noexcept { return mirrorWriteFailed.load (std::memory_order_acquire); }

private:
    // Constructed small and resized by start(), which is the only place the
    // real channel count and rate are known. RingBuffer::reset reallocates, so
    // it must never be called while the audio thread is running.
    std::atomic<bool> mixOnly { false };
    std::atomic<bool> cardWriteFailed { false };
    std::atomic<bool> mirrorWriteFailed { false };

    RingBuffer ring { 1 };
    std::vector<std::unique_ptr<SessionWriter>> stemWriters;
    std::unique_ptr<SessionWriter> mixWriter;

    // §6.3 mirror: a second, independent set of writers fed from the same
    // drained frames, so the copy is byte-identical without a second read.
    std::vector<std::unique_ptr<SessionWriter>> mirrorStemWriters;
    std::unique_ptr<SessionWriter> mirrorMixWriter;
    std::atomic<bool> mirroring { false };
    MixBusLimiter mixLimiter;

    // Written by the writer thread, read by the UI. Guarded rather than atomic
    // because it is a whole object with vectors in it, and the read is a UI
    // tick rather than anything on a deadline.
    std::unique_ptr<LoudnessMeter> loudness;
    mutable std::mutex loudnessLock;

    std::vector<float> trimGains;      // linear, from WriteChannelSpec::trimDb
    std::vector<std::atomic<bool>> channelLive;

    std::thread writerThread;
    std::atomic<bool> running { false };
    std::atomic<uint64_t> framesAccepted { 0 };
    std::atomic<uint64_t> framesDropped { 0 };

    /// Loudest sample written this take; reset by start().
    std::atomic<float> peakWritten { 0.0f };

    int numChannels = 0;
    double sampleRate = 48000.0;

    // Audio-thread scratch, allocated at start(). pushBlock interleaves into
    // this and hands the ring whole chunks, so the callback pays two memcpys per
    // chunk instead of one indexed store, one modulo and one release-store per
    // sample per channel.
    static constexpr size_t kInterleaveChunkFrames = 512;
    std::vector<float> interleaveScratch;

    // Writer-thread scratch, allocated at start() so the drain loop does not.
    std::vector<float> drainBuffer;
    std::vector<float> stemScratch;
    std::vector<float> mixScratch;

    void runWriterThread();
    void drainOnce (bool finalFlush);
    bool openMirrorWriters (const std::string& mirrorFolder,
                            const std::vector<WriteChannelSpec>& channels,
                            double rate, int bitDepth,
                            const std::string& originTimestamp);
};

} // namespace mma
