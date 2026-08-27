#include "TestFramework.h"
#include "Core/CpuPressureMonitor.h"

using namespace mma;

TEST_CASE (CpuPressureMonitor_QuietBelowThreshold)
{
    CpuPressureMonitor m;
    REQUIRE (m.update (0.5, false, 0.0) == PerformanceWarning::None);
    REQUIRE (m.update (0.5, false, 100.0) == PerformanceWarning::None);
}

TEST_CASE (CpuPressureMonitor_BriefSpikeDoesNotWarn)
{
    CpuPressureMonitor m;
    m.update (0.95, false, 0.0);
    // Well short of the 30-second sustain window.
    REQUIRE (m.update (0.95, false, 5.0) == PerformanceWarning::None);
}

TEST_CASE (CpuPressureMonitor_WarnsAfterSustainedPressure)
{
    CpuPressureMonitor m;
    m.update (0.95, false, 0.0);
    REQUIRE (m.update (0.95, false, CpuPressureMonitor::kSustainedSeconds) == PerformanceWarning::SustainedCpuPressure);
}

TEST_CASE (CpuPressureMonitor_WarnsOnlyOnceWhilePressureContinues)
{
    CpuPressureMonitor m;
    m.update (0.95, false, 0.0);
    REQUIRE (m.update (0.95, false, 30.0) == PerformanceWarning::SustainedCpuPressure);
    REQUIRE (m.update (0.95, false, 31.0) == PerformanceWarning::None);
    REQUIRE (m.update (0.95, false, 90.0) == PerformanceWarning::None);
}

TEST_CASE (CpuPressureMonitor_DipBelowThresholdRestartsTheClock)
{
    CpuPressureMonitor m;
    m.update (0.95, false, 0.0);
    m.update (0.95, false, 25.0);

    // Recovered, so the accumulated time no longer counts.
    m.update (0.10, false, 26.0);

    m.update (0.95, false, 27.0);
    REQUIRE (m.update (0.95, false, 50.0) == PerformanceWarning::None);
    REQUIRE (m.update (0.95, false, 57.0) == PerformanceWarning::SustainedCpuPressure);
}

TEST_CASE (CpuPressureMonitor_ExactlyAtThresholdIsNotOverIt)
{
    CpuPressureMonitor m;
    m.update (CpuPressureMonitor::kPressureThreshold, false, 0.0);
    REQUIRE (m.update (CpuPressureMonitor::kPressureThreshold, false, 60.0) == PerformanceWarning::None);
}

TEST_CASE (CpuPressureMonitor_ThermalThrottlingWarnsImmediately)
{
    CpuPressureMonitor m;
    // The OS is reporting it, not inferring it, so there is nothing to sustain.
    REQUIRE (m.update (0.10, true, 0.0) == PerformanceWarning::ThermalThrottling);
}

TEST_CASE (CpuPressureMonitor_ThermalWarningDoesNotRepeatWhileThrottled)
{
    CpuPressureMonitor m;
    REQUIRE (m.update (0.10, true, 0.0) == PerformanceWarning::ThermalThrottling);
    REQUIRE (m.update (0.10, true, 1.0) == PerformanceWarning::None);
}

TEST_CASE (CpuPressureMonitor_ThermalWarningRearmsAfterRecovery)
{
    CpuPressureMonitor m;
    REQUIRE (m.update (0.10, true, 0.0) == PerformanceWarning::ThermalThrottling);
    m.update (0.10, false, 10.0);
    REQUIRE (m.update (0.10, true, 20.0) == PerformanceWarning::ThermalThrottling);
}

TEST_CASE (CpuPressureMonitor_ReportsTimeOverThreshold)
{
    CpuPressureMonitor m;
    REQUIRE_NEAR (m.getSecondsOverThreshold (0.0), 0.0, 1e-9);

    m.update (0.95, false, 10.0);
    REQUIRE_NEAR (m.getSecondsOverThreshold (25.0), 15.0, 1e-9);

    m.update (0.10, false, 26.0);
    REQUIRE_NEAR (m.getSecondsOverThreshold (30.0), 0.0, 1e-9);
}

TEST_CASE (CpuPressureMonitor_ResetClearsState)
{
    CpuPressureMonitor m;
    m.update (0.95, false, 0.0);
    m.update (0.95, false, 30.0);

    m.reset();

    m.update (0.95, false, 100.0);
    REQUIRE (m.update (0.95, false, 130.0) == PerformanceWarning::SustainedCpuPressure);
}
