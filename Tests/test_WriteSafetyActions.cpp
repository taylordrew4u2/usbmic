#include "TestFramework.h"
#include "Core/WriteSafetyActions.h"

using namespace mma;

namespace {
constexpr int64_t kGB = 1024LL * 1024 * 1024;

MirrorPolicy activeMirror()
{
    MirrorPolicy p;
    p.evaluateAtArm (10 * kGB, 1 * kGB);
    return p;
}
}

TEST_CASE (WriteSafetyActions_FallsBackToMixOnlyAtNinetyPercent)
{
    CapacityMonitor capacity;
    MirrorPolicy mirror;
    WriteSafetyInputs in;
    in.recording = true;
    in.ringFillFraction = 0.95;

    const auto d = decideWriteSafetyActions (in, capacity, mirror);
    REQUIRE (d.fallBackToMixOnly);
    REQUIRE_FALSE (d.stopMirroring);
}

TEST_CASE (WriteSafetyActions_DoesNotFallBackTwice)
{
    CapacityMonitor capacity;
    MirrorPolicy mirror;
    WriteSafetyInputs in;
    in.recording = true;
    in.alreadyMixOnly = true;
    in.ringFillFraction = 0.99;

    REQUIRE_FALSE (decideWriteSafetyActions (in, capacity, mirror).fallBackToMixOnly);
}

TEST_CASE (WriteSafetyActions_LeavesStemsAloneBelowNinetyPercentOrWhenIdle)
{
    CapacityMonitor capacity;
    MirrorPolicy mirror;
    WriteSafetyInputs in;
    in.recording = true;
    in.ringFillFraction = 0.6;
    REQUIRE_FALSE (decideWriteSafetyActions (in, capacity, mirror).fallBackToMixOnly);

    in.recording = false;
    in.ringFillFraction = 0.95;
    REQUIRE_FALSE (decideWriteSafetyActions (in, capacity, mirror).fallBackToMixOnly);
}

TEST_CASE (WriteSafetyActions_DoesNotSpendTheRemainingTimeWarnings)
{
    // Called on every poll, including the ones where a warning returns first.
    // It must not consume the latched ten/two-minute warnings.
    CapacityMonitor capacity;
    MirrorPolicy mirror;
    WriteSafetyInputs in;
    in.recording = true;
    in.ringFillFraction = 0.95;

    for (int i = 0; i < 10; ++i)
        decideWriteSafetyActions (in, capacity, mirror);

    REQUIRE (capacity.evaluateRemaining (500.0) == RemainingTimeWarning::TenMinutes);
}

TEST_CASE (WriteSafetyActions_StopsTheMirrorBelowOneGigabyte)
{
    CapacityMonitor capacity;
    auto mirror = activeMirror();
    WriteSafetyInputs in;
    in.recording = true;
    in.mirroring = true;
    in.mirrorFreeBytes = 900LL * 1024 * 1024;

    const auto d = decideWriteSafetyActions (in, capacity, mirror);
    REQUIRE (d.stopMirroring);
    REQUIRE (mirror.wasStoppedForSpace());

    // Once stopped, a later poll must not ask for a second stop.
    REQUIRE_FALSE (decideWriteSafetyActions (in, capacity, mirror).stopMirroring);
}

TEST_CASE (WriteSafetyActions_KeepsTheMirrorWhenFreeSpaceIsUnknownOrAmple)
{
    CapacityMonitor capacity;
    auto mirror = activeMirror();
    WriteSafetyInputs in;
    in.recording = true;
    in.mirroring = true;

    in.mirrorFreeBytes = -1;
    REQUIRE_FALSE (decideWriteSafetyActions (in, capacity, mirror).stopMirroring);

    in.mirrorFreeBytes = 5 * kGB;
    REQUIRE_FALSE (decideWriteSafetyActions (in, capacity, mirror).stopMirroring);
    REQUIRE (mirror.isMirroring());
}
