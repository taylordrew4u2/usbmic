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
/// Every channel is steered that way, the §3.1 clock master included. The clock
/// this stream is pulled by belongs to the output device, so exempting a
/// microphone from correction does not make it the timebase -- it only leaves
/// that one channel uncorrected against a clock it has no relationship to.
///
/// Both ends are audio threads, so nothing here allocates, locks or blocks
/// after prepare() (§11). It is SPSC: the device callback is the only producer,
/// the output callback the only consumer.
class DeviceInputStream
{
public:
    /// How deep the platform layer asks the driver's own capture ring to be,
    /// in blocks. A reader thread that wakes late finds its audio still
    /// there rather than dropped in the kernel, and then hands it over all at
    /// once: this many blocks can land in this ring between two pulls.
    static constexpr int kSourceBufferBlocks = 8;

    /// Jitter headroom. Sized to take a whole driver ring's burst on top of
    /// the target fill with room to spare: with eight blocks here and eight
    /// in the driver, a stall the driver rode out overflowed this ring by two
    /// blocks -- audio the hardware had kept, thrown away one layer up. This
    /// costs no latency (the target fill below is what the monitor path pays)
    /// and 32 KB a channel at the largest block.
    static constexpr int kRingBlocks = 16;

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

    static_assert (kSourceBufferBlocks + kPreRollBlocks + 2 <= kRingBlocks,
                   "the ring must hold a full driver burst above its target fill, with a block "
                   "of delivery jitter and one in flight");

    /// §3.1/§3.3: a drift figure is only claimed once this much of it has been
    /// measured. Before that the estimate's resolution is too coarse to name.
    static constexpr double kMeasurementSeconds = 60.0;

    /// The clock the loop reads to place a pull within the device's block. A
    /// test may substitute simulated time; production leaves it alone.
    using NowNsFn = int64_t (*)();
    static void setClockForTesting (NowNsFn fn) noexcept;

    /// Test and tuning hook: false drives the loop from the raw ring level at
    /// pull time -- the whole-block staircase -- instead of the de-quantized
    /// level. Exists so Tools/sim_drift_loop.cpp can show the difference.
    static void setVirtualFillForTesting (bool enabled) noexcept;

    /// Test and tuning hook: this stream's loop gains, after prepare().
    void setLoopGainsForTesting (double kp, double ki) noexcept { compensator.setGains (kp, ki); }
    void setLoopSlewForTesting (double ppmPerSecond) noexcept { compensator.setSlewForTesting (ppmPerSecond); }

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
    bool hasStarted() const noexcept { return started.load (std::memory_order_relaxed); }

    /// §6.5: an unplugged mic keeps its channel and yields silence.
    ///
    /// Coming back is not the mirror image of going away. While the channel is
    /// dead this stream stops consuming, so its ring, its resampler phase and
    /// its PI loop all still hold the moment the microphone left. Resuming from
    /// that state replays audio from before the gap, or -- with a dry ring --
    /// holds the last sample before it as a DC offset for the rest of the take
    /// while counting every block as an underrun. So a false-to-true transition
    /// arms a restart, which the next pull() performs on the audio thread.
    void setLive (bool live) noexcept
    {
        if (live && ! channelLive.exchange (true, std::memory_order_relaxed))
            restartPending.store (true, std::memory_order_release);
        else
            channelLive.store (live, std::memory_order_relaxed);

        if (! live)
        {
            driftPpm.store (0.0, std::memory_order_relaxed);
            excessDrift.store (false, std::memory_order_relaxed);
            driftReportingResetEpoch.fetch_add (1, std::memory_order_release);
        }
    }

    bool isLive() const noexcept { return channelLive.load (std::memory_order_relaxed); }

    /// §0.1: samples this stream threw away because its ring was full -- the
    /// consumer stopped pulling, or pulled too slowly. Counted, never silent.
    uint64_t getOverrunSamples() const noexcept { return overrunSamples.load (std::memory_order_relaxed); }

    /// The loop's own correction against the clock that pulls this stream:
    /// what the resampler is doing right now. Positive means this device is
    /// being drained faster than nominal. This is a control state, not a
    /// measurement -- it slews at 5 PPM/s, overshoots, and is disturbed by
    /// every lost block -- so §3.3's reported figure comes from
    /// getMeasuredDriftPpm() instead.
    double getDriftPpm() const noexcept { return driftPpm.load (std::memory_order_relaxed); }
    bool hasSustainedExcessDrift() const noexcept { return excessDrift.load (std::memory_order_relaxed); }

    /// §3.3: this device's clock against the clock that pulls it, measured.
    /// A least-squares fit of samples-delivered against samples-pulled over
    /// the reporting window, so it does not depend on where the loop is in its
    /// transient, and a lost block moves it by a few PPM rather than sending
    /// it to a clamp. Zero, and hasDriftMeasurement() false, until
    /// kMeasurementSeconds of window exist. Positive means the device runs
    /// fast. The figure §3.3 shows is this minus the master's; the
    /// coordinator does that subtraction.
    double getMeasuredDriftPpm() const noexcept { return measuredPpm.load (std::memory_order_relaxed); }
    bool hasDriftMeasurement() const noexcept { return measured.load (std::memory_order_relaxed); }
    double getMeasurementSeconds() const noexcept { return measurementSeconds.load (std::memory_order_relaxed); }

    /// Samples the output clock asked for and the ring could not supply. Any
    /// value above zero is audio that was not there when it was needed.
    uint64_t getUnderrunSamples() const noexcept { return underruns.load (std::memory_order_relaxed); }

    /// §5.4: blocks in which this ring lost audio -- a pull that ran dry or a
    /// push that found the ring full -- as events rather than samples. The
    /// buffer ladder counts events: three inside thirty seconds is its trigger.
    uint64_t getLossEvents() const noexcept { return lossEvents.load (std::memory_order_relaxed); }

    /// Samples the device has delivered, dropped ones included, and samples
    /// the consumer has pulled since playout started. The measurement above
    /// is built from these; harnesses read them directly.
    uint64_t getPushedSamples() const noexcept { return pushedSamples.load (std::memory_order_relaxed); }
    uint64_t getPulledSamples() const noexcept { return pulledSamples.load (std::memory_order_relaxed); }

    /// The two clocks against the wall clock, from the same fit: how fast the
    /// device delivers and how fast the consumer pulls, each in PPM against
    /// nominal. Diagnostics; zero until measured.
    double getMeasuredDeviceRatePpm() const noexcept { return deviceRatePpm.load (std::memory_order_relaxed); }
    double getMeasuredConsumerRatePpm() const noexcept { return consumerRatePpm.load (std::memory_order_relaxed); }

    double getFillFraction() const noexcept { return ring.fillFraction(); }

    /// §3.3 drift reporting runs on a slower cadence than the audio callback,
    /// so the measurement and the sustained-excess flag are advanced from
    /// there. referencePpm is the clock master's own measured drift, since
    /// §3.3 judges each device against the master rather than against the
    /// output stream. Called from one thread only; never from an audio thread.
    void tickDriftReporting (double elapsedSeconds, double referencePpm = 0.0) noexcept;

private:
    RingBuffer ring;
    DriftCompensator compensator;
    double rate = 48000.0;

    std::atomic<bool> channelLive { true };

    std::atomic<uint64_t> overrunSamples { 0 };

    // Set on the message thread when a channel comes back, consumed by the
    // audio thread in pull(). The reset itself has to happen there: it touches
    // the resampler state and the ring's read index, both of which belong to
    // the consumer.
    std::atomic<bool> restartPending { false };
    std::atomic<double> driftPpm { 0.0 };
    std::atomic<bool> excessDrift { false };
    std::atomic<uint64_t> driftReportingResetEpoch { 0 };
    std::atomic<uint64_t> underruns { 0 };
    std::atomic<uint64_t> lossEvents { 0 };

    // Producer-published: when its last block landed and how big it was. The
    // consumer uses them to place its pull within the device's block, which
    // is what turns the ring level from a staircase in whole blocks into a
    // line the loop can follow. Sample counts feed the measurement.
    std::atomic<int64_t> lastPushNs { 0 };
    std::atomic<int> lastPushSamples { 0 };
    std::atomic<uint64_t> pushedSamples { 0 };
    std::atomic<int64_t> lastPullNs { 0 };
    std::atomic<uint64_t> pulledSamples { 0 };

    // Consumer-owned: the smoothed, de-quantized fill the loop is driven by.
    double fillAverage = 0.0;
    bool fillAverageValid = false;
    static constexpr double kFillSmoothing = 1.0 / 32.0;

    // Consumer-owned: silence this stream has written in place of audio that
    // had not arrived, not yet answered by the audio that arrives late for
    // the same span. A dry pull consumes nothing, so when the device's blocks
    // do land -- a late reader hands over everything the driver held at once
    // -- the ring holds that much more than before. The loop used to read
    // that as "device fast" and answer with the clamp, which on a slow device
    // drained the ring dry again: a limit cycle paid in one dropped block
    // every few seconds. Draining it slowly instead kept this channel that
    // far behind every other for as long as the drain took, minutes, with
    // the monitor path that much slower and the headroom that much smaller.
    //
    // The span the late audio covers already stands in the file as silence.
    // So it is skipped, up to the silence it answers for, at the first pull
    // that finds the ring above its target: the channel is back in step at
    // once, the loss was counted once, as the silence, and the loop never
    // sees it. Silence that nothing arrives to answer within a ring's worth
    // of pulls was audio lost outright -- a device that dropped it itself --
    // and is forgotten rather than left to eat the next block that runs a
    // few samples ahead.
    double silenceOwed = 0.0;
    int pullsSinceSilence = 0;

    // Reporting-thread-owned. The audio threads publish counters atomically;
    // they never touch anything below.
    double excessDriftSeconds = 0.0;
    uint64_t observedDriftReportingResetEpoch = 0;
    std::atomic<double> measuredPpm { 0.0 };
    std::atomic<bool> measured { false };
    std::atomic<double> measurementSeconds { 0.0 };
    std::atomic<double> deviceRatePpm { 0.0 };
    std::atomic<double> consumerRatePpm { 0.0 };

    // The measurement window: at each tick, each side's sample count paired
    // with the timestamp of the block that brought it there, in a fixed ring
    // so the reporting thread allocates nothing after construction either.
    // Each side is fitted against its own timestamps. Fitting one count
    // against the other would put the ring level into the residual, and the
    // level's slow swing through the loop's transient reads as slope over a
    // sixty-second window -- tens of PPM of bias at larger blocks.
    static constexpr int kMaxWindowPoints = 1024;
    struct RatePoint { double pushSeconds; double pushed; double pullSeconds; double pulled; };
    RatePoint window[kMaxWindowPoints] {};
    int windowStart = 0;
    int windowCount = 0;

    static constexpr double kExcessDriftThresholdPpm = 100.0;
    static constexpr double kExcessDriftSustainSeconds = 10.0;

    // Linear-interpolation resampler state. Two samples and a phase is all a
    // ratio this close to 1.0 needs, and it costs no allocation in the callback.
    float previousSample = 0.0f;
    float currentSample = 0.0f;
    double phase = 0.0;
    bool primed = false;
    // Read by the reporting thread too, to know whether there is anything to
    // measure yet; the consumer alone writes it.
    std::atomic<bool> started { false };

    size_t targetFillSamples = 0;

    bool readOne (float& out) noexcept;
    void resetMeasurementWindow() noexcept;
    double virtualFillNow (size_t available) const noexcept;
    double fillErrorNow (size_t available) noexcept;
    void noteSilence (int samples) noexcept;
    void skipLateAudio (int numSamples) noexcept;
};

} // namespace mma
