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
    /// briefly runs late without the ring ever emptying or overflowing.
    static constexpr int kRingBlocks = 8;

    /// How full the ring must be before playout starts, and what the PI loop
    /// steers back to. This is the latency the drift buffer costs, so it is
    /// deliberately small: §5.4 allows 10 ms end to end for the whole monitor
    /// path, and an input block plus this plus an output block has to fit
    /// inside that. Two blocks (2.7 ms at 64 samples / 48 kHz) absorbs the
    /// jitter between a device callback and the output callback without
    /// spending the budget on buffering.
    ///
    /// Sizing this from the ring capacity instead -- half full, say -- ties the
    /// latency to the headroom, so making the ring safer would silently make
    /// the monitor path slower.
    static constexpr int kPreRollBlocks = 2;

    explicit DeviceInputStream (double sampleRate) noexcept;

    /// Sizes the ring and clears all loop state. Not real-time safe -- call
    /// before the streams open.
    void prepare (double sampleRate, int bufferSizeSamples);

    /// Producer: this device's audio callback. Real-time safe.
    void pushBlock (const float* samples, int numSamples) noexcept;

    /// Consumer: the output clock pulls numSamples of this device's audio,
    /// resampled by the current drift ratio so it lands on the master's
    /// timebase. Real-time safe.
    ///
    /// Until the ring has pre-rolled to its target fill this yields silence and
    /// counts nothing: at stream start the output callback runs before any
    /// input has arrived, and consuming an empty ring there would glitch the
    /// first block of every take and leave the loop chasing a fill error that
    /// only means "not buffered yet". Genuine underruns after that are counted.
    void pull (float* destination, int numSamples) noexcept;

    /// True once enough audio has arrived to start consuming (§3.2 pre-roll).
    bool hasStarted() const noexcept { return started; }

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
    bool started = false;

    size_t targetFillSamples = 0;

    bool readOne (float& out) noexcept;
};

} // namespace mma
