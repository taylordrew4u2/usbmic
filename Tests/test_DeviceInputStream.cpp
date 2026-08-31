#include "TestFramework.h"
#include "Core/DeviceInputStream.h"
#include <cmath>
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

TEST_CASE (DeviceInputStream_StarvedBlockHoldsRatherThanClicking)
{
    // The held-sample fill must cover the whole remainder of the block. A gap of
    // stale or zeroed samples in the middle of an otherwise-held block is the
    // click the hold exists to avoid.
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    std::vector<float> in (128, 0.75f);
    s.pushBlock (in.data(), 128);

    std::vector<float> out (64, -99.0f);
    s.pull (out.data(), 64);
    s.pull (out.data(), 64);
    s.pull (out.data(), 64);   // by here the ring is dry

    for (float sample : out)
        REQUIRE (std::abs (sample - 0.75f) < 1e-4f);
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
