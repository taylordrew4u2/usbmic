#pragma once
#include <atomic>

namespace mma {

/// §8.1/§8.2 meter ballistics: 10ms attack, 1.5s decay, peak hold 2s then 20dB/s
/// decay, clip latch at 3 consecutive samples >= -0.1dBFS. Audio thread calls
/// pushBlock() (writes only atomics, no locks/allocation); UI thread polls at 60Hz.
class Metering
{
public:
    static constexpr float kMinDb = -60.0f;
    static constexpr float kMaxDb = 0.0f;
    static constexpr float kClipThresholdDb = -0.1f;
    static constexpr int kClipConsecutiveSamples = 3;
    static constexpr double kAttackSeconds = 0.010;
    static constexpr double kDecaySeconds = 1.5;
    static constexpr double kPeakHoldSeconds = 2.0;
    static constexpr double kPeakDecayDbPerSecond = 20.0;

    explicit Metering (double sampleRate) noexcept;

    /// Audio-thread call: pass this channel's raw samples for one block. Computes
    /// max-abs (and tracks a running count of consecutive samples at/above the
    /// clip threshold, needed for the §8.1 3-sample clip latch) then stores results
    /// into atomics only -- no allocation, locking, logging, or file I/O.
    void processAudioBlock (const float* samples, int numSamples) noexcept;

    /// Convenience overload when the caller has already reduced the block to a
    /// single max-abs value (e.g. a synthetic test, or a caller who doesn't need
    /// the sample-accurate clip latch).
    void pushBlockStats (float maxAbsLinear, int numSamplesInBlock) noexcept;

    /// UI-thread call at 60Hz: advances envelope/peak-hold ballistics by dtSeconds
    /// and returns the current displayed level in dBFS.
    float tick (double dtSeconds) noexcept;

    float getDisplayedLevelDb() const noexcept { return displayedDb; }
    float getPeakHoldDb() const noexcept { return peakHoldDb; }
    bool isClipped() const noexcept { return clipLatched.load (std::memory_order_relaxed); }
    int getClipCount() const noexcept { return clipCount.load (std::memory_order_relaxed); }

    /// UI calls this when the user taps the clip indicator to clear the latch.
    void acknowledgeClip() noexcept;

private:
    double sampleRate;

    // Precomputed at construction rather than evaluated per block on the audio
    // thread. MonitorBus already hoists its ceiling out of the callback for the
    // same reason; this is the same std::pow of the same kind of constant.
    float clipThresholdLinear;

    // Written by the audio thread, read by the UI thread.
    std::atomic<float> latestBlockPeakDb { kMinDb };
    std::atomic<int> consecutiveClipSamples { 0 };
    std::atomic<bool> clipLatched { false };
    std::atomic<int> clipCount { 0 };

    // UI-thread-owned ballistics state.
    float displayedDb = kMinDb;
    float peakHoldDb = kMinDb;
    double peakHoldElapsed = kPeakHoldSeconds; // starts "expired" so it decays immediately if no signal

    static float linearToDb (float linear) noexcept;
};

} // namespace mma
