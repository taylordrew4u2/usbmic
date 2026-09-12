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

TEST_CASE (DeviceInputStream_SustainedExcessDriftIsFlagged)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    runClockRatio (s, 128, 64, 60000);

    // §3.3: past 100 PPM sustained, the device is reported as unreliable rather
    // than quietly corrected forever.
    for (int i = 0; i < 20; ++i)
        s.tickDriftReporting (1.0);

    REQUIRE (s.hasSustainedExcessDrift());
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

    // kPreRollBlocks of kRingBlocks is 0.25, and settling sits just under it.
    // Nowhere near the 0.875 the uncorrected channel used to reach.
    REQUIRE (s.getFillFraction() < 0.3);
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
