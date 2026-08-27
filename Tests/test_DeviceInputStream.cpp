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

    // Start the ring above its target fill, which is what a device running
    // fast produces. §3.2 must then raise the playout ratio to drain it.
    std::vector<float> preload (1024, 0.25f);
    s.pushBlock (preload.data(), 1024);

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

    // The mirror image: a ring sitting below target means the device is not
    // keeping up, so the correction must go the other way.
    std::vector<float> preload (128, 0.25f);
    s.pushBlock (preload.data(), 128);

    runClockRatio (s, 64, 64, 400);

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

TEST_CASE (DeviceInputStream_UnderrunIsCountedNotFaked)
{
    DeviceInputStream s (48000.0);
    s.prepare (48000.0, 64);

    std::vector<float> out (64, 1.0f);

    // Nothing was ever pushed. §0.1 treats missing audio as the one unacceptable
    // failure, so it must be counted rather than silently papered over.
    s.pull (out.data(), 64);

    REQUIRE (s.getUnderrunSamples() > 0);
    REQUIRE_NEAR (out[0], 0.0f, 1e-9);
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
