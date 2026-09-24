#include "TestFramework.h"
#include "Core/DeviceInputStream.h"
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

using namespace mma;

namespace {

/// Feeds `pushSamplesPerBlock` in and pulls `pullSamplesPerBlock` out, repeatedly,
/// the way a device running at a slightly different clock rate would.
double runClockRatio (DeviceInputStream& s, int pushPerBlock, int pullPerBlock, int blocks)
{
    std::vector<float> in (static_cast<size_t> (pushPerBlock), 0.25f);
    std::vector<float> out (static_cast<size_t> (pullPerBlock), 0.0f);

    for (int i = 0; i < blocks; ++i)
    {
        s.pushBlock (in.data(), pushPerBlock);
        s.pull (out.data(), pullPerBlock);
    }

    return s.getDriftPpm();
}

struct AlignmentResult
{
    long long spreadSamples = std::numeric_limits<long long>::max();
    uint64_t underrunSamples = 0;
};

constexpr int kStartupAlignmentBlock = 64;

AlignmentResult runStartupAlignment (double seconds, double rate)
{
    constexpr std::array<double, 4> offsets { 40.0, 100.0, -80.0, 45.0 };

    struct Clock
    {
        double ppm = 0.0;
        DeviceInputStream stream { 48000.0 };
        double sampleDebt = 0.0;
        long long pushed = 0;
        long long markerSourceIndex = -1;
        long long markerOutputIndex = -1;
        std::vector<float> input = std::vector<float> (kStartupAlignmentBlock + 1, 0.0f);
    };

    std::array<Clock, offsets.size()> clocks;
    for (size_t i = 0; i < clocks.size(); ++i)
    {
        clocks[i].ppm = offsets[i];
        clocks[i].stream.prepare (rate, kStartupAlignmentBlock);
    }

    const auto totalBlocks = static_cast<long long> (seconds * rate / kStartupAlignmentBlock);
    const auto markerBlock = totalBlocks
                           - static_cast<long long> (30.0 * rate / kStartupAlignmentBlock);
    std::vector<float> output (kStartupAlignmentBlock, 0.0f);
    long long outputIndex = 0;

    for (long long blockIndex = 0; blockIndex < totalBlocks; ++blockIndex)
    {
        for (auto& clock : clocks)
        {
            clock.sampleDebt += kStartupAlignmentBlock * clock.ppm * 1.0e-6;
            int inputSamples = kStartupAlignmentBlock;

            if (clock.sampleDebt >= 1.0)
            {
                ++inputSamples;
                clock.sampleDebt -= 1.0;
            }
            else if (clock.sampleDebt <= -1.0)
            {
                --inputSamples;
                clock.sampleDebt += 1.0;
            }

            std::fill (clock.input.begin(), clock.input.begin() + inputSamples, 0.0f);

            if (blockIndex == markerBlock)
            {
                clock.input[0] = 1.0f;
                clock.markerSourceIndex = clock.pushed;
            }

            clock.stream.pushBlock (clock.input.data(), inputSamples);
            clock.pushed += inputSamples;
        }

        for (auto& clock : clocks)
        {
            clock.stream.pull (output.data(), kStartupAlignmentBlock);

            if (clock.markerSourceIndex >= 0 && clock.markerOutputIndex < 0)
            {
                for (int sample = 0; sample < kStartupAlignmentBlock; ++sample)
                {
                    if (output[static_cast<size_t> (sample)] > 0.05f)
                    {
                        clock.markerOutputIndex = outputIndex + sample;
                        break;
                    }
                }
            }
        }

        outputIndex += kStartupAlignmentBlock;
    }

    AlignmentResult result;
    long long earliest = std::numeric_limits<long long>::max();
    long long latest = std::numeric_limits<long long>::min();

    for (const auto& clock : clocks)
    {
        if (clock.markerOutputIndex < 0)
            return result;

        earliest = std::min (earliest, clock.markerOutputIndex);
        latest = std::max (latest, clock.markerOutputIndex);
        result.underrunSamples += clock.stream.getUnderrunSamples();
    }

    result.spreadSamples = latest - earliest;
    return result;
}

} // namespace

TEST_CASE (DeviceInputStream_EveryChannelIsCorrectedIncludingTheClockMaster)
{
    // This class has no notion of a master any more, and that is the fix.
    //
    // The stream a DeviceInputStream is pulled by belongs to the output device,
    // not to any microphone, so exempting the §3.1 master from correction never
    // made it the timebase -- it left one channel uncorrected against a clock it
    // had no relationship to. Its ring then walked to one end of its travel and
    // stayed there, dropping arrivals when full or holding its last sample when
    // dry: drift, on the one channel the rig quotes every other against.
    //
    // §3.1's reference is now a reporting role, held by CaptureCoordinator. Down
    // here every channel is steered the same way.
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    // A ring deliberately far from target fill: whoever this channel is, the
    // loop must answer it.
    runClockRatio (s, 96, 64, 40);

    REQUIRE (s.getDriftPpm() > 0.0);
}

TEST_CASE (DeviceInputStream_FastDeviceIsPulledDown)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    // Start the ring well above its target fill, which is what a device running
    // fast produces. §3.2 must then raise the playout ratio to drain it.
    std::vector<float> preload (400, 0.25f);
    s.pushBlock (preload.data(), 400);

    // Thereafter the device supplies exactly what the clock consumes, so the
    // surplus persists and only the loop can remove it.
    runClockRatio (s, 64, 64, 400);

    REQUIRE (s.getDriftPpm() > 0.0);
}

TEST_CASE (DeviceInputStream_SlowDeviceIsPulledUp)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    // Enough to clear pre-roll and start, then a device that supplies less than
    // the clock consumes: the ring drains below target, which is what "not
    // keeping up" looks like, and the correction must go the other way.
    std::vector<float> preload (256, 0.25f);
    s.pushBlock (preload.data(), 256);

    runClockRatio (s, 60, 64, 400);

    REQUIRE (s.getDriftPpm() < 0.0);
}

TEST_CASE (DeviceInputStream_CorrectionStaysInsideTheSafetyClamp)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    // A device this far out is broken, not drifting. §3.2 clamps at +/-200 PPM
    // so a bad measurement can never turn into an audible pitch shift. The
    // clamp is what is under test, so this runs long enough to reach it: the
    // 5 PPM/s slew is deliberately slow, which is the point.
    runClockRatio (s, 128, 64, 60000);

    REQUIRE (std::abs (s.getDriftPpm()) <= DriftCompensator::kMaxRatioDeviationPpm + 1e-9);
}

TEST_CASE (DeviceInputStream_PreRollYieldsSilenceWithoutCryingUnderrun)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    std::vector<float> out (64, 1.0f);

    // The output clock starts before any device has delivered. That is normal
    // startup, not lost audio: consuming here would click at the top of every
    // take, and counting it would make the §0.1 metric untrustworthy.
    s.pull (out.data(), 64);

    REQUIRE_FALSE (s.hasStarted());
    REQUIRE (s.getUnderrunSamples() == 0);
    REQUIRE_NEAR (out[0], 0.0f, 1e-9);
}

TEST_CASE (DeviceInputStream_StartsOnceThePreRollTargetIsReached)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    // One block is not enough to start on: it would leave the loop chasing a
    // fill error that only means "not buffered yet", which reads as drift.
    std::vector<float> small (64, 0.5f);
    s.pushBlock (small.data(), 64);

    std::vector<float> out (64, 0.0f);
    s.pull (out.data(), 64);
    REQUIRE_FALSE (s.hasStarted());

    // §5.4: playout starts at kPreRollBlocks, which is the latency this buffer
    // costs the monitor path -- not some fraction of the ring's headroom.
    std::vector<float> rest (64, 0.5f);
    s.pushBlock (rest.data(), 64);
    s.pull (out.data(), 64);

    REQUIRE (s.hasStarted());
    REQUIRE_NEAR (out[0], 0.5f, 1e-4);
}

TEST_CASE (DeviceInputStream_UnderrunAfterStartingIsCountedNotFaked)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    // Get past pre-roll, then starve it.
    std::vector<float> in (256, 0.5f);
    s.pushBlock (in.data(), 256);

    std::vector<float> out (256, 0.0f);
    s.pull (out.data(), 256);
    REQUIRE (s.hasStarted());

    s.pull (out.data(), 256);
    s.pull (out.data(), 256);

    // Audio the clock asked for and the device never delivered is exactly the
    // failure §0.1 refuses to let pass silently.
    REQUIRE (s.getUnderrunSamples() > 0);
}

TEST_CASE (DeviceInputStream_UnpluggedDeviceYieldsSilenceNotStaleAudio)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    std::vector<float> in (512, 0.5f);
    s.pushBlock (in.data(), 512);

    // §6.5: the channel survives the mic leaving. What it must not do is keep
    // replaying whatever was in the ring when the mic vanished.
    s.setLive (false);

    std::vector<float> out (64, 1.0f);
    s.pull (out.data(), 64);

    for (float sample : out)
        REQUIRE_NEAR (sample, 0.0f, 1e-9);
}

TEST_CASE (DeviceInputStream_PassesAudioThroughAtMatchedClocks)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    std::vector<float> in (256, 0.5f);
    s.pushBlock (in.data(), 256);

    std::vector<float> out (64, 0.0f);
    s.pull (out.data(), 64);

    // A constant signal in must be that same constant out: the resampler is
    // only allowed to change timing, never level.
    for (float sample : out)
        REQUIRE_NEAR (sample, 0.5f, 1e-4);

    REQUIRE (s.getUnderrunSamples() == 0);
}

namespace {

// A simulated clock for the tests that need the loop to know where a pull
// falls in the device's block, and the measurement to know when each block
// arrived. Advanced by the test, one block per push.
int64_t simulatedNs = 0;
int64_t simulatedClock() { return simulatedNs; }

struct ScopedSimulatedClock
{
    ScopedSimulatedClock() { simulatedNs = 0; DeviceInputStream::setClockForTesting (simulatedClock); }
    ~ScopedSimulatedClock() { DeviceInputStream::setClockForTesting ([]() -> int64_t { return 0; }); }
};

} // namespace

TEST_CASE (DeviceInputStream_SustainedExcessDriftIsFlagged)
{
    ScopedSimulatedClock clock;
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    // A device delivering 130 samples per 64 pulled is 2,000,000 PPM fast --
    // absurd, and exactly what "unreliable" is for. Ticked as it runs, since
    // §3.3's figure is measured over a minute of running, not read off the
    // loop.
    std::vector<float> in (130, 0.25f), out (64, 0.0f);
    const int64_t blockNs = 64 * 1000000000LL / 48000;

    for (int second = 0; second < 80; ++second)
    {
        for (int i = 0; i < 750; ++i)
        {
            simulatedNs += blockNs;
            s.pushBlock (in.data(), 130);
            simulatedNs += blockNs / 2;
            s.pull (out.data(), 64);
            simulatedNs -= blockNs / 2;
        }

        s.tickDriftReporting (1.0);
    }

    REQUIRE (s.hasDriftMeasurement());
    REQUIRE (s.getMeasuredDriftPpm() > 100.0);
    REQUIRE (s.hasSustainedExcessDrift());
}

TEST_CASE (DeviceInputStream_MeasuresADeviceClockWithinTwoPpmInAMinute)
{
    // §3.3's reported figure is a measurement of the device's clock against
    // the pulling clock, not the loop's state. A device 150 PPM fast, delivering
    // whole blocks at its own cadence against a pull clock at nominal, is
    // measured to within 2 PPM once the minute has run, whatever the loop is
    // doing meanwhile.
    ScopedSimulatedClock clock;
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    std::vector<float> in (64, 0.25f), out (64, 0.0f);
    const double pullPeriodS = 64.0 / 48000.0;
    const double pushPeriodS = pullPeriodS / (1.0 + 150.0e-6);
    double nextPushS = 0.0, nextPullS = pullPeriodS * 0.5, nextTickS = 0.1;
    double nowS = 0.0;

    // Long enough for the loop too: at the spec's 5 PPM/s slew, 150 PPM is
    // reached at 30 s and the level it accumulated meanwhile takes another
    // minute or so to drain through the clamp.
    while (nowS < 160.0)
    {
        if (nextPushS <= nextPullS)
        {
            nowS = nextPushS;
            simulatedNs = static_cast<int64_t> (nowS * 1.0e9);
            s.pushBlock (in.data(), 64);
            nextPushS += pushPeriodS;
        }
        else
        {
            nowS = nextPullS;
            simulatedNs = static_cast<int64_t> (nowS * 1.0e9);
            s.pull (out.data(), 64);
            nextPullS += pullPeriodS;
        }

        if (nowS >= nextTickS)
        {
            s.tickDriftReporting (0.1);
            nextTickS += 0.1;
        }
    }

    REQUIRE (s.hasDriftMeasurement());
    REQUIRE_NEAR (s.getMeasuredDriftPpm(), 150.0, 2.0);
    REQUIRE (s.getMeasurementSeconds() >= 60.0);
    REQUIRE_NEAR (s.getMeasuredDeviceRatePpm(), 150.0, 2.0);
    REQUIRE_NEAR (s.getMeasuredConsumerRatePpm(), 0.0, 2.0);
    // And the loop itself, on a level it can follow, has locked close to the
    // clock rather than to a clamp.
    REQUIRE_NEAR (s.getDriftPpm(), 150.0, 15.0);
    REQUIRE (s.getUnderrunSamples() <= 64);
}

TEST_CASE (DeviceInputStream_CountsALossEventPerBlockNotPerSample)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    // Pre-roll, then pull twice from a ring that has one block: the second
    // pull runs dry part way through -- one event, not one per sample.
    std::vector<float> in (128, 0.25f), out (64, 0.0f);
    s.pushBlock (in.data(), 128);
    s.pull (out.data(), 64);
    s.pull (out.data(), 64);
    s.pull (out.data(), 64);

    REQUIRE (s.getUnderrunSamples() > 0);
    REQUIRE (s.getLossEvents() >= 1);
    REQUIRE (s.getLossEvents() <= 2);

    // A push that finds the ring full is one event too.
    const auto before = s.getLossEvents();
    std::vector<float> flood (static_cast<size_t> (DeviceInputStream::kRingBlocks + 1) * 64, 0.25f);
    s.pushBlock (flood.data(), static_cast<int> (flood.size()));
    REQUIRE (s.getOverrunSamples() > 0);
    REQUIRE (s.getLossEvents() == before + 1);
}

TEST_CASE (DeviceInputStream_ReconnectDoesNotKeepPreGapDriftCredit)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    runClockRatio (s, 128, 64, 60000);
    s.tickDriftReporting (9.0);
    REQUIRE_FALSE (s.hasSustainedExcessDrift());

    // Reconnection resets the audio-thread compensator. Its reporting-thread
    // sustain window must reset as well; otherwise the first two seconds of a
    // new connection combine with nine seconds from the old device instance.
    s.setLive (false);
    s.setLive (true);
    std::vector<float> out (64, 0.0f);
    s.pull (out.data(), 64); // consumes restartPending on the audio side
    runClockRatio (s, 128, 64, 60000);

    s.tickDriftReporting (0.0); // consume the audio thread's reset epoch
    s.tickDriftReporting (2.0);
    REQUIRE_FALSE (s.hasSustainedExcessDrift());
}

TEST_CASE (DeviceInputStream_DriftReportingCanRunWhileAudioIsPulled)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);
    std::atomic<bool> start { false };
    const std::vector<float> input (64, 0.25f);

    std::thread producer ([&] {
        while (! start.load (std::memory_order_acquire)) {}
        for (int i = 0; i < 5000; ++i)
            s.pushBlock (input.data(), static_cast<int> (input.size()));
    });

    std::thread consumer ([&] {
        std::vector<float> output (64, 0.0f);
        while (! start.load (std::memory_order_acquire)) {}
        for (int i = 0; i < 5000; ++i)
            s.pull (output.data(), static_cast<int> (output.size()));
    });

    std::thread reporter ([&] {
        while (! start.load (std::memory_order_acquire)) {}
        for (int i = 0; i < 5000; ++i)
            s.tickDriftReporting (0.001);
    });

    start.store (true, std::memory_order_release);
    producer.join();
    consumer.join();
    reporter.join();

    REQUIRE (std::isfinite (s.getDriftPpm()));
}

TEST_CASE (DeviceInputStream_UnderrunCountNeverExceedsWhatWasAskedFor)
{
    // Regression: when the ring ran dry mid-block, pull() broke out of the
    // resampler's inner loop and let the outer loop run on. The ring was still
    // dry on the next sample, so it re-entered the failure path and added
    // (numSamples - i) again -- once per remaining sample. A single starved
    // 64-sample block was reported as ~2,000 lost samples.
    //
    // §0.1 makes any non-zero underrun the one failure the user is shown, so an
    // inflated count is a false alarm about the app's central promise. The count
    // can never exceed the number of samples actually requested.
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    // Enough to clear pre-roll (2 blocks) and prime the resampler, and no more.
    std::vector<float> in (128, 0.5f);
    s.pushBlock (in.data(), 128);

    std::vector<float> out (64, 0.0f);

    const int blocks = 20;
    for (int i = 0; i < blocks; ++i)
        s.pull (out.data(), 64);   // starves after the first couple of blocks

    const uint64_t requested = static_cast<uint64_t> (blocks) * 64;
    REQUIRE (s.getUnderrunSamples() <= requested);
}

TEST_CASE (DeviceInputStream_StarvedBlockFallsToSilenceInsteadOfInventingDC)
{
    // A dry input used to hold the final non-zero sample over every missing
    // frame. That turns one captured value into a sustained DC offset in both
    // the stem and the headphones. Missing audio is represented honestly as
    // silence and by the underrun counter; it is never synthesized from stale
    // state.
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    std::vector<float> in (128, 0.75f);
    s.pushBlock (in.data(), 128);

    std::vector<float> out (64, -99.0f);

    // Find the first partial block that reaches the end of the buffered audio.
    // Its valid prefix may still contain captured samples; after it returns the
    // ring and interpolator are both known to be dry.
    for (int attempts = 0; attempts < 8 && s.getUnderrunSamples() == 0; ++attempts)
        s.pull (out.data(), 64);

    REQUIRE (s.getUnderrunSamples() > 0);

    for (int dryBlock = 0; dryBlock < 2; ++dryBlock)
    {
        const auto before = s.getUnderrunSamples();
        std::fill (out.begin(), out.end(), -99.0f);
        s.pull (out.data(), 64);

        for (float sample : out)
            REQUIRE_NEAR (sample, 0.0f, 1e-9);

        REQUIRE (s.getUnderrunSamples() - before == 64);
    }
}

TEST_CASE (DeviceInputStream_DriftLoopIsDrivenByItsOwnRingNotByAnyOtherChannel)
{
    // What "locked to the master" amounts to in this path: no channel reads any
    // other channel's audio. A stream's ratio comes from its own ring fill
    // against the pull it is given, and nothing else enters that arithmetic.
    //
    // This is why an unplugged clock master cannot poison the other channels --
    // there is no path from it to them -- and it is the fact the mid-take
    // failover question turns on, so it is pinned here rather than re-derived.
    DeviceInputStream fast (48000.0), alsoFast (48000.0);
    fast.prepare (48000.0, 64);
    alsoFast.prepare (48000.0, 64);

    std::vector<float> preload (400, 0.25f);
    fast.pushBlock (preload.data(), 400);
    alsoFast.pushBlock (preload.data(), 400);

    runClockRatio (fast, 64, 64, 400);
    runClockRatio (alsoFast, 64, 64, 400);

    // Two streams that never saw each other, or any master, settle identically:
    // the loop has no other input than this stream's own fill.
    REQUIRE_NEAR (fast.getDriftPpm(), alsoFast.getDriftPpm(), 1e-12);
    REQUIRE (fast.getDriftPpm() > 0.0);
}

TEST_CASE (DeviceInputStream_NamingAChannelTheReferenceDoesNotStopCorrectingIt)
{
    // Regression, and the whole point of the change. Under the old exemption a
    // channel promoted to master stopped being steered: its reported drift
    // froze and its ring was left to run wherever its crystal took it. That
    // made mid-take failover a hazard rather than a fix.
    //
    // There is nothing to promote here now -- the reference lives in
    // CaptureCoordinator and this class corrects unconditionally -- so a
    // channel's correction cannot be switched off behind its back. Running the
    // same imbalance twice as long simply carries the loop further.
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    std::vector<float> preload (400, 0.25f);
    s.pushBlock (preload.data(), 400);
    runClockRatio (s, 64, 64, 400);

    const double afterSettling = s.getDriftPpm();
    REQUIRE (afterSettling > 0.0);

    runClockRatio (s, 64, 64, 400);

    // Still being steered: the loop kept working rather than freezing.
    REQUIRE (s.getDriftPpm() > 0.0);
}

TEST_CASE (DeviceInputStream_RingIsHeldAtTargetEvenAtAWideClockOffset)
{
    // The cost the exemption used to carry, now measured as its absence. A mic
    // 100 PPM off the clock that pulls it, run for five simulated minutes.
    //
    // Exempt from correction, this ring ended pinned near full (0.875) -- and at
    // that limit RingBuffer::write keeps only what fits, so arriving audio was
    // being dropped a sample at a time with the underrun counter reading zero
    // throughout. Corrected, it parks at its pre-roll target and stays there.
    //
    // Five minutes because §3.2 caps the loop's slew at 5 PPM/s: reaching
    // 100 PPM and settling takes a couple of minutes, and sampling before then
    // measures the slew rather than the steady state.
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    std::vector<float> pre (256, 0.25f);
    s.pushBlock (pre.data(), 256);

    // 100 PPM fast: an extra sample arrives roughly one block in every 156.
    std::vector<float> in (65, 0.25f), out (64, 0.0f);
    double owed = 0.0;

    const int blocks = static_cast<int> (300.0 * 48000.0 / 64.0);

    for (int i = 0; i < blocks; ++i)
    {
        owed += 64.0 * 1.0001;
        const int n = static_cast<int> (owed);
        owed -= n;
        s.pushBlock (in.data(), n);
        s.pull (out.data(), 64);
    }

    // kPreRollBlocks of kRingBlocks is 0.125, and settling sits just over it.
    // Nowhere near the top of the ring, where the uncorrected channel used to
    // sit.
    REQUIRE (s.getFillFraction() < 0.25);
    REQUIRE (s.getUnderrunSamples() == 0);
}

TEST_CASE (DeviceInputStream_WideClockOffsetsSettleWithinOneMillisecondBy72Seconds)
{
    // Regression for L3: with the original PI gains these same four deliberately
    // wide offsets were still 57 samples (1.188 ms) apart at the 42-second
    // marker in a 72-second run. That is startup phase debt, not long-term
    // accumulation: the four-hour gate settled, but an ordinary short take did
    // not yet meet the same alignment ceiling.
    for (const double rate : { 44100.0, 48000.0 })
    {
        const auto result = runStartupAlignment (72.0, rate);

        REQUIRE (result.underrunSamples == 0);
        REQUIRE (static_cast<double> (result.spreadSamples) / rate * 1000.0 < 1.0);
    }
}

TEST_CASE (DeviceInputStream_ReconnectedChannelReturnsToSilenceNotToAStuckSample)
{
    // §6.5 logs both halves of a mid-take unplug: the channel goes silent, and
    // it comes back. Coming back was the half that was never implemented down
    // here.
    //
    // While the channel is dead pull() returns early, so nothing consumes the
    // ring and nothing touches the resampler. Everything below still describes
    // the instant the microphone left: the interpolator holding the last sample
    // before the gap, primed and started both true. An unplugged USB device's
    // stream does not come back with it -- the OS hands out a new one -- so the
    // ring stays dry, and resuming from that state used to hold that last
    // sample as a DC offset for the rest of the take while counting every block
    // as lost audio.
    //
    // Two failures in one: a constant offset in the stem and the monitor mix,
    // and an underrun count climbing at the sample rate, which §0.1 shows the
    // user as the one failure this app promises not to have.
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    // Run normally at a healthy DC level, so "the last sample before the gap"
    // is unmistakably not zero.
    std::vector<float> in (64, 0.5f), out (64, 0.0f);

    for (int i = 0; i < 40; ++i)
    {
        s.pushBlock (in.data(), 64);
        s.pull (out.data(), 64);
    }

    REQUIRE (out[32] > 0.4f); // it really was running

    // The mic leaves. §6.5: the channel stays and yields silence.
    s.setLive (false);

    for (int i = 0; i < 20; ++i)
        s.pull (out.data(), 64);

    REQUIRE (out[32] == 0.0f);

    const auto underrunsWhileDead = s.getUnderrunSamples();

    // The mic comes back, but its old stream is gone with it: nothing is
    // delivering into this ring any more.
    s.setLive (true);

    for (int i = 0; i < 200; ++i)
        s.pull (out.data(), 64);

    // Silence, not the sample it was holding when the microphone left.
    for (int i = 0; i < 64; ++i)
        REQUIRE (out[i] == 0.0f);

    // And nothing counted as lost: audio that never arrived was not audio the
    // ring failed to supply. Pre-roll is exactly the state a stream that has
    // not delivered yet should be in.
    REQUIRE (s.getUnderrunSamples() == underrunsWhileDead);
}

TEST_CASE (DeviceInputStream_ReconnectedChannelDoesNotReplayAudioFromBeforeTheGap)
{
    // The other half of the same bug. When the ring does still hold audio from
    // before the unplug -- a device that stopped being consumed rather than one
    // that vanished -- resuming played that stale audio out after the gap, in
    // the wrong place in the take by however long the microphone was away.
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    std::vector<float> stale (64, -0.75f), fresh (64, 0.25f), out (64, 0.0f);

    for (int i = 0; i < 10; ++i)
    {
        s.pushBlock (stale.data(), 64);
        s.pull (out.data(), 64);
    }

    s.setLive (false);

    // Audio from before the gap, still sitting in the ring.
    for (int i = 0; i < 4; ++i)
        s.pushBlock (stale.data(), 64);

    s.setLive (true);

    // The device starts delivering again. Every sample from here on is either
    // pre-roll silence or the audio arriving now -- stale is negative and fresh
    // is positive, so a single negative sample is audio from before the gap
    // played back after it.
    bool sawStale = false;
    bool sawFresh = false;

    for (int block = 0; block < 40; ++block)
    {
        s.pushBlock (fresh.data(), 64);
        s.pull (out.data(), 64);

        for (int i = 0; i < 64; ++i)
        {
            sawStale = sawStale || out[static_cast<size_t> (i)] < -1.0e-6f;
            sawFresh = sawFresh || out[static_cast<size_t> (i)] > 1.0e-6f;
        }
    }

    REQUIRE_FALSE (sawStale);
    REQUIRE (sawFresh); // and it did resume, rather than staying silent forever
}

TEST_CASE (DeviceInputStream_OverrunsAreCountedNotSwallowed)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    std::vector<float> block (64, 0.5f);
    REQUIRE (s.getOverrunSamples() == 0);

    // Twice the ring's worth with no pull between: half is thrown away, and
    // this used to throw the count away with it.
    for (int i = 0; i < DeviceInputStream::kRingBlocks * 2; ++i)
        s.pushBlock (block.data(), 64);

    REQUIRE (s.getOverrunSamples() > 0);
    REQUIRE (s.getOverrunSamples() < static_cast<uint64_t> (DeviceInputStream::kRingBlocks) * 2 * 64);
}

namespace {

// A ramp: each sample's value is its index in the source, so the output says
// which source sample it came from. Exact in a float up to 2^24 samples.
struct RampSource
{
    long long pushed = 0;
    std::vector<float> block;

    explicit RampSource (int n) : block (static_cast<size_t> (n)) {}

    void push (DeviceInputStream& s, int n)
    {
        if (block.size() < static_cast<size_t> (n))
            block.resize (static_cast<size_t> (n));

        for (int i = 0; i < n; ++i)
            block[static_cast<size_t> (i)] = static_cast<float> (pushed + i);
        s.pushBlock (block.data(), n);
        pushed += n;
    }
};

// How far the first sample of a pull is behind the newest delivered sample.
// While a channel is in step this is constant to within a sample or two of
// resampler phase; a channel playing late audio shows it grown by the gap.
double stepBehind (const RampSource& src, const std::vector<float>& out)
{
    return static_cast<double> (src.pushed) - static_cast<double> (out[0]);
}

constexpr int64_t kBlockNs64 = 64 * 1000000000LL / 48000;

// Pre-roll so the ring sits at the loop's target from the first pull: two
// blocks plus the half block a pull lands after a delivery, less the sample
// the interpolator primes with. The loop itself, at 5 PPM/s, would take a
// minute to walk it there.
void primeAtTarget (DeviceInputStream& s, RampSource& src, std::vector<float>& out)
{
    simulatedNs = 1'000'000'000; // zero is what the clock hook reads as "nothing delivered yet"
    src.push (s, 64 + 64 + 32 + 1);
    s.pull (out.data(), 64);
}

// One block delivered, one pulled half a block later, for `blocks` blocks.
void runInStep (DeviceInputStream& s, RampSource& src, std::vector<float>& out, int blocks)
{
    for (int i = 0; i < blocks; ++i)
    {
        src.push (s, 64);
        simulatedNs += kBlockNs64 / 2;
        s.pull (out.data(), 64);
        simulatedNs += kBlockNs64 - kBlockNs64 / 2;
    }
}

} // namespace

TEST_CASE (DeviceInputStream_RingTakesAWholeDriverBurstOnTopOfItsTargetFill)
{
    // The platform layer asks the driver for kSourceBufferBlocks periods of
    // ring, so a reader thread that wakes late finds its audio still there --
    // and then hands all of it over at once. With eight blocks here and eight
    // there, that burst overflowed this ring by two blocks: audio the
    // hardware had kept, thrown away one layer up.
    ScopedSimulatedClock clock;
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    RampSource src (64);
    std::vector<float> out (64, 0.0f);
    primeAtTarget (s, src, out);
    runInStep (s, src, out, 1500);
    REQUIRE (s.getUnderrunSamples() == 0);
    const double inStep = stepBehind (src, out);

    // The whole process stalls. The device thread wakes first and hands over
    // the driver's ring; the output clock has not caught up yet.
    for (int i = 0; i < DeviceInputStream::kSourceBufferBlocks; ++i)
        src.push (s, 64);
    simulatedNs += kBlockNs64 * DeviceInputStream::kSourceBufferBlocks;

    REQUIRE (s.getOverrunSamples() == 0);

    // Then the output clock catches up, back to back.
    for (int i = 0; i < DeviceInputStream::kSourceBufferBlocks; ++i)
        s.pull (out.data(), 64);

    REQUIRE (s.getUnderrunSamples() == 0);
    REQUIRE (s.getOverrunSamples() == 0);

    // Nothing was lost and nothing is out of step.
    runInStep (s, src, out, 4);
    REQUIRE_NEAR (stepBehind (src, out), inStep, 2.0);
}

TEST_CASE (DeviceInputStream_LateAudioAfterSilenceIsSkippedSoTheChannelStaysInStep)
{
    ScopedSimulatedClock clock;
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    RampSource src (64);
    std::vector<float> out (64, 0.0f);
    primeAtTarget (s, src, out);
    runInStep (s, src, out, 1500);
    REQUIRE (s.getUnderrunSamples() == 0);
    const double inStep = stepBehind (src, out);
    const double loopBeforeStall = s.getDriftPpm();

    // The device's thread stalls for eight blocks while the output clock
    // keeps pulling: the ring runs dry and silence stands in for the audio.
    for (int i = 0; i < 8; ++i)
    {
        simulatedNs += kBlockNs64 / 2;
        s.pull (out.data(), 64);
        simulatedNs += kBlockNs64 - kBlockNs64 / 2;
    }

    const auto silence = s.getUnderrunSamples();
    REQUIRE (silence > 0);

    // It wakes and hands over everything the driver held, at once, and then
    // carries on at pace.
    for (int i = 0; i < 8; ++i)
        src.push (s, 64);
    REQUIRE (s.getOverrunSamples() == 0);

    simulatedNs += kBlockNs64 / 2;
    s.pull (out.data(), 64);
    simulatedNs += kBlockNs64 - kBlockNs64 / 2;
    runInStep (s, src, out, 4);

    // The span that arrived late already stands in the file as silence.
    // Playing it too would put this channel eight blocks behind every other
    // for as long as the loop took to drain them; instead it is skipped and
    // the channel is back in step at once.
    REQUIRE_NEAR (stepBehind (src, out), inStep, 2.0);

    // Counted once: the silence is the loss. The skip is not a second one.
    REQUIRE (s.getUnderrunSamples() == silence);
    REQUIRE (s.getOverrunSamples() == 0);

    // And the loop was never shown the burst as fill: it is where the stall
    // found it, not slewing off six blocks of false error towards the clamp.
    REQUIRE_NEAR (s.getDriftPpm(), loopBeforeStall, 2.0);
}

TEST_CASE (DeviceInputStream_SilenceNothingArrivesLateForIsWrittenOff)
{
    // A device that dropped the audio itself -- an xrun in the driver, a USB
    // hiccup -- resumes at pace with nothing late behind it. The silence it
    // cost must not sit waiting to eat the next block that runs a few samples
    // ahead.
    ScopedSimulatedClock clock;
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    RampSource src (64);
    std::vector<float> out (64, 0.0f);
    primeAtTarget (s, src, out);
    runInStep (s, src, out, 1500);

    for (int i = 0; i < 8; ++i)
    {
        simulatedNs += kBlockNs64 / 2;
        s.pull (out.data(), 64);
        simulatedNs += kBlockNs64 - kBlockNs64 / 2;
    }
    REQUIRE (s.getUnderrunSamples() > 0);

    // Back at pace, with a cushion but no burst: two blocks is the target,
    // not an excess over it.
    src.push (s, 64);
    src.push (s, 64);
    simulatedNs += kBlockNs64 / 2;
    s.pull (out.data(), 64);
    simulatedNs += kBlockNs64 - kBlockNs64 / 2;

    // Every block from here on is continuous with the one before it: nothing
    // is being skipped.
    float lastValue = out[63];
    for (int i = 0; i < DeviceInputStream::kRingBlocks * 2; ++i)
    {
        runInStep (s, src, out, 1);
        REQUIRE (out[0] - lastValue < 4.0f);
        lastValue = out[63];
    }

    // Long after the silence, one block runs ahead -- ordinary jitter. It is
    // fill for the loop to drain over seconds, not late audio to skip.
    src.push (s, 64);
    runInStep (s, src, out, 1);
    REQUIRE (out[0] - lastValue < 4.0f);
}

TEST_CASE (DeviceInputStream_ARingOneBlockDeepIsNotAStarvationInEveryBlock)
{
    // The interpolator carries a pair of samples between blocks, so the
    // block that primes it reads one sample more than it writes. A ring
    // holding exactly one block per pull -- what a stall leaves behind, and
    // what the loop takes half a minute to lift at 200 PPM -- used to run
    // one sample short on that block, write one sample of silence, de-prime,
    // and prime again on the next: a step to zero and a phase reset in
    // every block for as long as it lasted, on a take whose record showed a
    // sample of loss per block. On a fixture microphone that was every other
    // 64-sample block voting for the wrong tone.
    ScopedSimulatedClock clock;
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 128);

    simulatedNs = 1'000'000'000;
    RampSource src (128);
    std::vector<float> out (128, 0.0f);

    // Pre-roll to the target, then let a stall drain it to one block: the
    // device delivers, the pull consumes, and the ring never gets deeper.
    src.push (s, 256);
    s.pull (out.data(), 128);
    s.pull (out.data(), 128);
    REQUIRE (s.getUnderrunSamples() == 0);

    const int64_t blockNs = 128 * 1000000000LL / 48000;
    float last = out[127];

    for (int i = 0; i < 400; ++i)
    {
        src.push (s, 128);
        simulatedNs += blockNs / 2;
        s.pull (out.data(), 128);
        simulatedNs += blockNs - blockNs / 2;

        // Continuous: the ramp carries on from where the last block left it,
        // give or take one held sample, and never steps to zero.
        REQUIRE (out[0] - last <= 2.0f);
        REQUIRE (out[0] - last >= 0.0f);
        for (int k = 1; k < 128; ++k)
            REQUIRE (out[k] - out[k - 1] >= 0.0f);
        last = out[127];
    }

    REQUIRE (s.getUnderrunSamples() == 0);
    REQUIRE (s.getLossEvents() == 0);
}
