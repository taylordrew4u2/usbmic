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
