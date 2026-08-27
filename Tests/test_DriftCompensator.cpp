#include "TestFramework.h"
#include "Core/DriftCompensator.h"
#include <algorithm>
#include <cmath>

using namespace mma;

TEST_CASE (DriftCompensator_StartsAtUnity)
{
    DriftCompensator dc (48000.0);
    REQUIRE_NEAR (dc.getRatio(), 1.0, 1e-12);
    REQUIRE_NEAR (dc.getPpm(), 0.0, 1e-12);
}

TEST_CASE (DriftCompensator_PositiveFillErrorPushesRatioUp)
{
    DriftCompensator dc (48000.0);
    for (int i = 0; i < 1000; ++i)
        dc.update (1000.0, 64);
    REQUIRE (dc.getPpm() > 0.0);
}

TEST_CASE (DriftCompensator_ClampsToMax200Ppm)
{
    DriftCompensator dc (48000.0);
    for (int i = 0; i < 200000; ++i)
        dc.update (1.0e9, 64);
    REQUIRE (dc.getPpm() <= DriftCompensator::kMaxRatioDeviationPpm + 1e-6);
    REQUIRE (dc.getPpm() >= DriftCompensator::kMaxRatioDeviationPpm - 1e-6);
}

TEST_CASE (DriftCompensator_ClampsToMinNegative200Ppm)
{
    DriftCompensator dc (48000.0);
    for (int i = 0; i < 200000; ++i)
        dc.update (-1.0e9, 64);
    REQUIRE (dc.getPpm() <= -DriftCompensator::kMaxRatioDeviationPpm + 1e-6);
    REQUIRE (dc.getPpm() >= -DriftCompensator::kMaxRatioDeviationPpm - 1e-6);
}

TEST_CASE (DriftCompensator_NeverJumpsFasterThanSlewLimit)
{
    DriftCompensator dc (48000.0);
    const int blockSize = 64;
    const double blockSeconds = blockSize / 48000.0;
    const double maxStepPpm = DriftCompensator::kMaxSlewPpmPerSecond * blockSeconds;

    double previousPpm = dc.getPpm();
    for (int i = 0; i < 5000; ++i)
    {
        dc.update (1.0e9, blockSize); // huge error, forces the slew limiter to be the binding constraint
        double newPpm = dc.getPpm();
        REQUIRE (std::abs (newPpm - previousPpm) <= maxStepPpm + 1e-9);
        previousPpm = newPpm;
    }
}

TEST_CASE (DriftCompensator_ResetClearsState)
{
    DriftCompensator dc (48000.0);
    for (int i = 0; i < 1000; ++i)
        dc.update (1000.0, 64);
    REQUIRE (dc.getPpm() != 0.0);
    dc.reset();
    REQUIRE_NEAR (dc.getPpm(), 0.0, 1e-12);
}

TEST_CASE (DriftCompensator_SustainedExcessDriftFlagsAfterThreshold)
{
    DriftCompensator dc (48000.0);
    for (int i = 0; i < 200000; ++i)
        dc.update (1.0e9, 64); // drive ppm above the 100ppm excess-drift threshold
    REQUIRE (dc.getPpm() > 100.0);

    REQUIRE_FALSE (dc.isSustainedExcessDrift());
    dc.updateSustainedDriftFlag (5.0);
    REQUIRE_FALSE (dc.isSustainedExcessDrift());
    dc.updateSustainedDriftFlag (5.0);
    REQUIRE (dc.isSustainedExcessDrift());
}

TEST_CASE (DriftCompensator_SustainedExcessDriftFlagClearsWhenBackInRange)
{
    DriftCompensator dc (48000.0);
    for (int i = 0; i < 200000; ++i)
        dc.update (1.0e9, 64);
    dc.updateSustainedDriftFlag (10.0);
    REQUIRE (dc.isSustainedExcessDrift());

    dc.reset();
    dc.updateSustainedDriftFlag (10.0);
    REQUIRE_FALSE (dc.isSustainedExcessDrift());
}

TEST_CASE (DriftCompensator_SettlesInsteadOfOscillating)
{
    // The regression this exists for: the integral saturated at the +/-200 PPM
    // clamp long before the 5 PPM/s slew-limited output could reach it, so every
    // time the fill crossed target the loop had to unwind from saturation --
    // at 5 PPM/s. Over hours it swung between +140 and -45 PPM instead of
    // settling, and starved rings badly enough to underrun.
    DriftCompensator c (48000.0);

    // A device running 100 PPM fast: its ring fills, so the loop must raise the
    // playout ratio until the fill error it sees settles at a steady offset.
    double fill = 0.0;
    double lastPpm = 0.0;
    double maxLateSwing = 0.0;

    for (int block = 0; block < 1200000; ++block)
    {
        // Fill grows by the clock mismatch and shrinks by whatever correction
        // is currently applied -- the same feedback the real ring provides.
        fill += 64.0 * (100.0e-6 - c.getPpm() * 1.0e-6);
        c.update (fill, 64);

        // Once the loop has had ample time to settle, it must stay settled.
        if (block > 600000)
            maxLateSwing = std::max (maxLateSwing, std::abs (c.getPpm() - lastPpm));

        lastPpm = c.getPpm();
    }

    REQUIRE_NEAR (c.getPpm(), 100.0, 0.5);
    REQUIRE (maxLateSwing < 0.01);
}

TEST_CASE (DriftCompensator_DoesNotIntegrateWhileRateLimited)
{
    DriftCompensator a (48000.0);
    DriftCompensator b (48000.0);

    // A huge, sustained error drives the output hard against its slew limit.
    for (int i = 0; i < 50; ++i)
        a.update (1.0e6, 64);

    // The same loop reaching the same place gradually. If the integral had wound
    // up during the rate-limited climb, the first would now be carrying stored
    // correction the second is not, and would overshoot when the error clears.
    for (int i = 0; i < 50; ++i)
        b.update (200.0, 64);

    for (int i = 0; i < 2000; ++i) { a.update (0.0, 64); b.update (0.0, 64); }

    // With the error gone, both must return towards zero rather than one of them
    // continuing to push from a saturated integral.
    REQUIRE (a.getPpm() < 5.0);
    REQUIRE (b.getPpm() < 5.0);
}
