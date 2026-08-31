#include "TestFramework.h"
#include "Core/MirrorPolicy.h"

using namespace mma;

namespace {
constexpr int64_t kGB = 1024LL * 1024 * 1024;
}

TEST_CASE (MirrorPolicy_MirrorsWhenThereIsRoom)
{
    MirrorPolicy p;
    // 10 GB free, 1 GB session: comfortably past 2 GB plus the session.
    REQUIRE (p.evaluateAtArm (10 * kGB, 1 * kGB) == MirrorState::Active);
    REQUIRE (p.isMirroring());
}

TEST_CASE (MirrorPolicy_RefusesWhenSpaceIsOnlyJustEnough)
{
    MirrorPolicy p;
    // Exactly 2 GB plus the session is not "exceeds" (§6.3).
    REQUIRE (p.evaluateAtArm (MirrorPolicy::kMinHeadroomBytes + 1 * kGB, 1 * kGB)
             == MirrorState::NotStartedNoSpace);
}

TEST_CASE (MirrorPolicy_AccountsForTheProjectedSessionSize)
{
    MirrorPolicy p;
    // 5 GB free is plenty for a small take...
    REQUIRE (p.evaluateAtArm (5 * kGB, 1 * kGB) == MirrorState::Active);

    // ...and not enough for a big one, because the mirror needs room for the
    // whole session, not just the headroom.
    MirrorPolicy q;
    REQUIRE (q.evaluateAtArm (5 * kGB, 4 * kGB) == MirrorState::NotStartedNoSpace);
}

TEST_CASE (MirrorPolicy_StopsBelowOneGigabyteDuringATake)
{
    MirrorPolicy p;
    p.evaluateAtArm (10 * kGB, 1 * kGB);
    REQUIRE (p.isMirroring());

    REQUIRE (p.evaluateDuringRecording (MirrorPolicy::kStopBytes - 1) == MirrorState::StoppedLowSpace);
    REQUIRE_FALSE (p.isMirroring());
    REQUIRE (p.wasStoppedForSpace());
}

TEST_CASE (MirrorPolicy_KeepsMirroringAtExactlyOneGigabyte)
{
    MirrorPolicy p;
    p.evaluateAtArm (10 * kGB, 1 * kGB);
    // §6.3 says below 1 GB, so 1 GB itself still mirrors.
    REQUIRE (p.evaluateDuringRecording (MirrorPolicy::kStopBytes) == MirrorState::Active);
}

TEST_CASE (MirrorPolicy_NeverRestartsWithinTheSameTake)
{
    MirrorPolicy p;
    p.evaluateAtArm (10 * kGB, 1 * kGB);
    p.evaluateDuringRecording (100);
    REQUIRE (p.wasStoppedForSpace());

    // Space freed up again mid-take. Resuming would leave a hole in the copy,
    // and a partial mirror is not a usable one.
    REQUIRE (p.evaluateDuringRecording (50 * kGB) == MirrorState::StoppedLowSpace);
    REQUIRE_FALSE (p.isMirroring());
}

TEST_CASE (MirrorPolicy_AMirrorThatNeverStartedDoesNotStartMidTake)
{
    MirrorPolicy p;
    p.evaluateAtArm (1 * kGB, 1 * kGB);
    REQUIRE (p.getState() == MirrorState::NotStartedNoSpace);

    REQUIRE (p.evaluateDuringRecording (50 * kGB) == MirrorState::NotStartedNoSpace);
}

TEST_CASE (MirrorPolicy_UserDisableWins)
{
    MirrorPolicy p;
    p.setEnabledByUser (false);

    REQUIRE (p.evaluateAtArm (500 * kGB, 1 * kGB) == MirrorState::DisabledByUser);
    REQUIRE_FALSE (p.isMirroring());
}

TEST_CASE (MirrorPolicy_DisablingMidTakeStopsIt)
{
    MirrorPolicy p;
    p.evaluateAtArm (10 * kGB, 1 * kGB);
    REQUIRE (p.isMirroring());

    p.setEnabledByUser (false);
    REQUIRE_FALSE (p.isMirroring());
}

TEST_CASE (MirrorPolicy_StoppedForSpaceIsDistinctFromNeverStarted)
{
    // §6.3 asks for the mid-take stop to be noted in session.json, so the two
    // reasons must not collapse into one flag.
    MirrorPolicy stopped;
    stopped.evaluateAtArm (10 * kGB, 1 * kGB);
    stopped.evaluateDuringRecording (100);
    REQUIRE (stopped.wasStoppedForSpace());

    MirrorPolicy neverStarted;
    neverStarted.evaluateAtArm (1 * kGB, 1 * kGB);
    REQUIRE_FALSE (neverStarted.wasStoppedForSpace());
}

TEST_CASE (MirrorPolicy_ResetPreparesTheNextTake)
{
    MirrorPolicy p;
    p.evaluateAtArm (10 * kGB, 1 * kGB);
    p.evaluateDuringRecording (100);
    REQUIRE (p.wasStoppedForSpace());

    p.reset();
    REQUIRE_FALSE (p.wasStoppedForSpace());
    REQUIRE (p.evaluateAtArm (10 * kGB, 1 * kGB) == MirrorState::Active);
}

TEST_CASE (MirrorPolicy_theSettingIsReadableBeforeTheFirstArm)
{
    MirrorPolicy policy;

    // The state starts at DisabledByUser and only becomes Active at arm time,
    // so a caller asking "will the next take get a second copy" cannot read the
    // state to find out -- before the first arm it would always say no, and the
    // path the user is told about would be missing from the one screen shown
    // before any file exists.
    REQUIRE (policy.getState() == MirrorState::DisabledByUser);
    REQUIRE (policy.isEnabledByUser());

    policy.setEnabledByUser (false);
    REQUIRE_FALSE (policy.isEnabledByUser());

    policy.setEnabledByUser (true);
    REQUIRE (policy.isEnabledByUser());
}

TEST_CASE (MirrorPolicy_AFailedWriteStopsTheMirrorAndIsReportedOnce)
{
    MirrorPolicy p;
    p.setEnabledByUser (true);
    REQUIRE (p.evaluateAtArm (100LL * 1024 * 1024 * 1024, 1024) == MirrorState::Active);

    // The transition is the event: the caller says it once rather than on
    // every poll for the rest of the take.
    REQUIRE (p.noteWriteFailure());
    REQUIRE_FALSE (p.noteWriteFailure());

    REQUIRE (p.getState() == MirrorState::StoppedWriteFailed);
    REQUIRE_FALSE (p.isMirroring());

    // §6.3 requires the stop be visible in session.json, and the two reasons
    // must not be confused for each other.
    REQUIRE (p.wasStoppedForWriteFailure());
    REQUIRE_FALSE (p.wasStoppedForSpace());
}

TEST_CASE (MirrorPolicy_AFailedWriteNeverRestartsTheMirror)
{
    MirrorPolicy p;
    p.setEnabledByUser (true);
    p.evaluateAtArm (100LL * 1024 * 1024 * 1024, 1024);
    REQUIRE (p.noteWriteFailure());

    // Plenty of room, and the volume may even be back -- but a copy with a
    // hole in the middle is not a usable copy, so it stays stopped.
    REQUIRE (p.evaluateDuringRecording (100LL * 1024 * 1024 * 1024)
             == MirrorState::StoppedWriteFailed);
    REQUIRE_FALSE (p.isMirroring());
}

TEST_CASE (MirrorPolicy_AMirrorThatNeverStartedCannotFail)
{
    // A write failure reported against a mirror that was never running is not
    // an event, and must not produce a notice or overwrite why it is not running.
    MirrorPolicy p;
    p.setEnabledByUser (false);
    REQUIRE_FALSE (p.noteWriteFailure());
    REQUIRE (p.getState() == MirrorState::DisabledByUser);

    MirrorPolicy q;
    q.setEnabledByUser (true);
    REQUIRE (q.evaluateAtArm (1024, 100LL * 1024 * 1024 * 1024) == MirrorState::NotStartedNoSpace);
    REQUIRE_FALSE (q.noteWriteFailure());
    REQUIRE (q.getState() == MirrorState::NotStartedNoSpace);
}

TEST_CASE (MirrorPolicy_ALowSpaceStopIsNotRelabelledAsAWriteFailure)
{
    MirrorPolicy p;
    p.setEnabledByUser (true);
    p.evaluateAtArm (100LL * 1024 * 1024 * 1024, 1024);
    REQUIRE (p.evaluateDuringRecording (1024) == MirrorState::StoppedLowSpace);

    // Writes to a stopped mirror can still fail; the reason the user is given
    // must stay the first one, which is the one that actually stopped it.
    REQUIRE_FALSE (p.noteWriteFailure());
    REQUIRE (p.wasStoppedForSpace());
    REQUIRE_FALSE (p.wasStoppedForWriteFailure());
}
