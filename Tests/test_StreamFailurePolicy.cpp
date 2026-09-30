#include "TestFramework.h"
#include "Platform/IAudioBackend.h"

using namespace mma;

TEST_CASE (StreamFailurePolicy_OnlyALiveRateChangeStopsTheTake)
{
    REQUIRE (streamFailureRequiresRecordingStop (StreamFailureKind::sampleRateChanged));
    REQUIRE_FALSE (streamFailureRequiresRecordingStop (StreamFailureKind::deviceUnavailable));
    REQUIRE_FALSE (streamFailureRequiresRecordingStop (StreamFailureKind::processorOverload));
    REQUIRE_FALSE (streamFailureRequiresRecordingStop (StreamFailureKind::safetyMonitoringUnavailable));
    REQUIRE_FALSE (streamFailureRequiresRecordingStop (StreamFailureKind::unknown));
}

TEST_CASE (StreamFailurePolicy_RecoverableSafetyReportsAreWarnings)
{
    REQUIRE (streamFailureIsWarning (StreamFailureKind::processorOverload));
    REQUIRE (streamFailureIsWarning (StreamFailureKind::safetyMonitoringUnavailable));
    REQUIRE_FALSE (streamFailureIsWarning (StreamFailureKind::sampleRateChanged));
    REQUIRE_FALSE (streamFailureIsWarning (StreamFailureKind::deviceUnavailable));
    REQUIRE_FALSE (streamFailureIsWarning (StreamFailureKind::unknown));
}

TEST_CASE (StreamFailurePolicy_ADeadInputStreamIsReopenedOnceThenLeftDead)
{
    // Idle: reopen now, or the next take records silence for that mic.
    REQUIRE (deadInputStreamAction (false, false) == DeadInputStreamAction::reopenNow);

    // Mid-take the channels are fixed; Stop owes the reopen.
    REQUIRE (deadInputStreamAction (true, false) == DeadInputStreamAction::reopenAtStop);

    // Died again straight after a reopen: no restart loop, it stays dead.
    REQUIRE (deadInputStreamAction (false, true) == DeadInputStreamAction::leaveDead);
    REQUIRE (deadInputStreamAction (true, true) == DeadInputStreamAction::leaveDead);
}

TEST_CASE (StreamFailurePolicy_AResumedStreamIsNeitherAWarningNorAStop)
{
    REQUIRE_FALSE (streamFailureIsWarning (StreamFailureKind::resumed));
    REQUIRE_FALSE (streamFailureRequiresRecordingStop (StreamFailureKind::resumed));
}

TEST_CASE (StreamFailurePolicy_AStreamResumesOnlyOnACallbackAfterItWasCalledDead)
{
    // Never called dead: nothing to take back.
    REQUIRE_FALSE (inputStreamHasResumed (0.0, 100.0, false));
    // Called dead at 50 s, last callback before that: still dead.
    REQUIRE_FALSE (inputStreamHasResumed (50.0, 44.0, false));
    REQUIRE_FALSE (inputStreamHasResumed (50.0, 50.0, false));
    // A callback after the report: it is delivering again.
    REQUIRE (inputStreamHasResumed (50.0, 50.5, false));
    // A device the OS says is gone is not revived by a stray callback.
    REQUIRE_FALSE (inputStreamHasResumed (50.0, 50.5, true));
}
