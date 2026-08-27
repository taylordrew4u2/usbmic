#pragma once
#include <atomic>
#include <cstdint>
#include "RingBuffer.h"
#include "DriftCompensator.h"

namespace mma {

/// One microphone's capture path (§3.2). Separate USB devices each run on their
/// own crystal, so their callbacks arrive on independent clocks and cannot be
/// treated as one aligned block. Each device therefore writes into its own
/// lock-free ring on its own audio thread, and the output clock pulls from every
/// ring at a resample ratio the PI loop keeps adjusting.
///
/// Both ends are audio threads, so nothing here allocates, locks or blocks
/// after prepare() (§11). It is SPSC: the device callback is the only producer,
/// the output callback the only consumer.
class DeviceInputStream
{
public:
    /// Jitter headroom. Eight output blocks is enough to absorb a device that
    /// briefly runs late without the ring emptying, while staying far below the
    /// §5.4 latency budget -- the loop keeps fill near the target, not near full.
    static constexpr int kRingBlocks = 8;
    static constexpr size_t kMinRingSamples = 2048;

    explicit DeviceInputStream (double sampleRate) noexcept;

    /// Sizes the ring and clears all loop state. Not real-time safe -- call
    /// before the streams open.
    void prepare (double sampleRate, int bufferSizeSamples);

    /// Producer: this device's audio callback. Real-time safe.
    void pushBlock (const float* samples, int numSamples) noexcept;

    /// Consumer: the output clock pulls numSamples of this device's audio,
    /// resampled by the current drift ratio so it lands on the master's
    /// timebase. Real-time safe. Writes silence for any sample the ring could
    /// not supply, and counts it.
    void pull (float* destination, int numSamples) noexcept;

    /// §3.1: the master defines the timebase, so it is never resampled. Its
    /// ratio stays exactly 1.0 no matter what its fill does.
    void setIsMaster (bool shouldBeMaster) noexcept { isMaster = shouldBeMaster; }
    bool getIsMaster() const noexcept { return isMaster; }

    /// §6.5: an unplugged mic keeps its channel and yields silence.
    void setLive (bool live) noexcept { channelLive.store (live, std::memory_order_relaxed); }
    bool isLive() const noexcept { return channelLive.load (std::memory_order_relaxed); }

    /// §3.3 reporting. Positive means this device runs fast relative to the master.
    double getDriftPpm() const noexcept { return driftPpm.load (std::memory_order_relaxed); }
    bool hasSustainedExcessDrift() const noexcept { return excessDrift.load (std::memory_order_relaxed); }

    /// Samples the output clock asked for and the ring could not supply. Any
    /// value above zero is audio that was not there when it was needed.
    uint64_t getUnderrunSamples() const noexcept { return underruns.load (std::memory_order_relaxed); }

    double getFillFraction() const noexcept { return ring.fillFraction(); }

    /// §3.3 drift reporting runs on a slower cadence than the audio callback,
    /// so the sustained-excess flag is advanced from there.
    void tickDriftReporting (double elapsedSeconds) noexcept;

private:
    RingBuffer ring;
    DriftCompensator compensator;

    bool isMaster = false;
    std::atomic<bool> channelLive { true };
    std::atomic<double> driftPpm { 0.0 };
    std::atomic<bool> excessDrift { false };
    std::atomic<uint64_t> underruns { 0 };

    // Linear-interpolation resampler state. Two samples and a phase is all a
    // ratio this close to 1.0 needs, and it costs no allocation in the callback.
    float previousSample = 0.0f;
    float currentSample = 0.0f;
    double phase = 0.0;
    bool primed = false;

    size_t targetFillSamples = 0;

    bool readOne (float& out) noexcept;
};

} // namespace mma
