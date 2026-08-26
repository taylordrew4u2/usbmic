#include "TestFramework.h"
#include "Core/DriftCompensator.h"

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
