#include "TestFramework.h"
#include "Core/CapacityMonitor.h"

using namespace mma;

TEST_CASE (CapacityMonitor_HealthyBelowTheWarningFill)
{
    CapacityMonitor m;
    REQUIRE (m.evaluateFill (0.49, true) == WritePipelineState::Healthy);
}

TEST_CASE (CapacityMonitor_WarnsAtHalfFill)
{
    CapacityMonitor m;
    REQUIRE (m.evaluateFill (CapacityMonitor::kFillWarningFraction, true) == WritePipelineState::FillWarning);
}

TEST_CASE (CapacityMonitor_DegradesOnlyWhenTheMirrorIsUnavailable)
{
    CapacityMonitor m;

    // With a mirror there is a second copy of the stems, so keep writing them.
    REQUIRE (m.evaluateFill (0.95, true) == WritePipelineState::FillWarning);

    // Without one, the mix file is what must survive.
    REQUIRE (m.evaluateFill (0.95, false) == WritePipelineState::DegradedToMixOnly);
}

TEST_CASE (CapacityMonitor_WarnsAtTenMinutesThenTwoMinutes)
{
    CapacityMonitor m;

    REQUIRE (m.evaluateRemaining (3600.0) == RemainingTimeWarning::None);
    REQUIRE (m.evaluateRemaining (CapacityMonitor::kTenMinutesSeconds) == RemainingTimeWarning::TenMinutes);
    REQUIRE (m.evaluateRemaining (CapacityMonitor::kTwoMinutesSeconds) == RemainingTimeWarning::TwoMinutes);
}

TEST_CASE (CapacityMonitor_EachWarningFiresExactlyOnce)
{
    CapacityMonitor m;

    REQUIRE (m.evaluateRemaining (500.0) == RemainingTimeWarning::TenMinutes);
    // A novice cannot act on the same warning sixty times a second.
    REQUIRE (m.evaluateRemaining (499.0) == RemainingTimeWarning::None);
    REQUIRE (m.evaluateRemaining (400.0) == RemainingTimeWarning::None);

    REQUIRE (m.evaluateRemaining (100.0) == RemainingTimeWarning::TwoMinutes);
    REQUIRE (m.evaluateRemaining (90.0) == RemainingTimeWarning::None);
}

TEST_CASE (CapacityMonitor_JumpingStraightPastTenMinutesDoesNotWarnLate)
{
    CapacityMonitor m;

    // A big drop in free space should not produce a ten-minute warning after the
    // two-minute one has already been shown.
    REQUIRE (m.evaluateRemaining (60.0) == RemainingTimeWarning::TwoMinutes);
    REQUIRE (m.evaluateRemaining (300.0) == RemainingTimeWarning::None);
    REQUIRE (m.hasWarnedTenMinutes());
}

TEST_CASE (CapacityMonitor_ReportsExhaustionOnceAtZero)
{
    CapacityMonitor m;

    REQUIRE (m.evaluateRemaining (0.0) == RemainingTimeWarning::Exhausted);
    REQUIRE (m.evaluateRemaining (0.0) == RemainingTimeWarning::None);
    REQUIRE (m.evaluateRemaining (-5.0) == RemainingTimeWarning::None);
}

TEST_CASE (CapacityMonitor_ExhaustionSuppressesEarlierWarnings)
{
    CapacityMonitor m;

    REQUIRE (m.evaluateRemaining (0.0) == RemainingTimeWarning::Exhausted);
    REQUIRE (m.evaluateRemaining (300.0) == RemainingTimeWarning::None);
    REQUIRE (m.evaluateRemaining (100.0) == RemainingTimeWarning::None);
}

TEST_CASE (CapacityMonitor_RecordsTheFirstDegradationSamplePosition)
{
    CapacityMonitor m;
    REQUIRE (m.getDegradationSamplePosition() == -1);

    m.noteDegradationAt (48000 * 137);
    REQUIRE (m.getDegradationSamplePosition() == 48000LL * 137);

    // §6.5 wants the position where degradation began, not the latest one.
    m.noteDegradationAt (48000LL * 900);
    REQUIRE (m.getDegradationSamplePosition() == 48000LL * 137);
}

TEST_CASE (CapacityMonitor_ResetClearsEverythingForTheNextTake)
{
    CapacityMonitor m;
    m.evaluateRemaining (100.0);
    m.noteDegradationAt (1234);

    m.reset();

    REQUIRE_FALSE (m.hasWarnedTenMinutes());
    REQUIRE_FALSE (m.hasWarnedTwoMinutes());
    REQUIRE (m.getDegradationSamplePosition() == -1);
    REQUIRE (m.evaluateRemaining (500.0) == RemainingTimeWarning::TenMinutes);
}

TEST_CASE (CapacityMonitor_ZeroSecondsLeftIsExhaustedNotUnknown)
{
    // The app's remaining-time figure used to collapse "the drive is full" and
    // "the drive could not be read" into the same negative sentinel, and every
    // consumer treats negative as "say nothing" -- so a genuinely full card
    // stopped nothing and warned nobody. Zero is now reachable, and this is
    // what the monitor must make of it.
    CapacityMonitor monitor;

    REQUIRE (monitor.evaluateRemaining (0.0) == RemainingTimeWarning::Exhausted);

    // Latched: the take is already being stopped, and a second announcement of
    // a drive that is still full helps nobody.
    REQUIRE (monitor.evaluateRemaining (0.0) == RemainingTimeWarning::None);
}

TEST_CASE (CapacityMonitor_AnExhaustedDriveDoesNotLaterEmitAStaleWarning)
{
    CapacityMonitor monitor;

    REQUIRE (monitor.evaluateRemaining (0.0) == RemainingTimeWarning::Exhausted);

    // Room appearing again must not produce a ten-minute warning about a drive
    // the user has already been told is full.
    REQUIRE (monitor.evaluateRemaining (500.0) == RemainingTimeWarning::None);
}
