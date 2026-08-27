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

TEST_CASE (DeviceInputStream_MasterIsNeverResampled)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);
    s.setIsMaster (true);

    // §3.1: the master defines the timebase. Even with its ring deliberately
    // far from the target fill, its rate must not be corrected -- correcting
    // the reference against itself is what a master exists to prevent.
    runClockRatio (s, 96, 64, 40);

    REQUIRE_NEAR (s.getDriftPpm(), 0.0, 1e-12);
}

TEST_CASE (DeviceInputStream_FastDeviceIsPulledDown)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);
    s.setIsMaster (false);

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
    s.setIsMaster (false);

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
    s.setIsMaster (false);

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
    s.setIsMaster (true);

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
    s.setIsMaster (false);

    runClockRatio (s, 128, 64, 60000);

    // §3.3: past 100 PPM sustained, the device is reported as unreliable rather
    // than quietly corrected forever.
    for (int i = 0; i < 20; ++i)
        s.tickDriftReporting (1.0);

    REQUIRE (s.hasSustainedExcessDrift());
}
