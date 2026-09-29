#include "TestFramework.h"
#include "Core/QuitGate.h"

using namespace mma;

TEST_CASE (QuitGate_NothingToFinishQuitsAtOnce)
{
    QuitGate gate;
    REQUIRE (gate.onQuitRequested (true, 0.0) == QuitGate::Decision::QuitNow);
    REQUIRE_FALSE (gate.isPending());
}

TEST_CASE (QuitGate_FilesStillFinishingWaitsThenQuitsWhenDone)
{
    QuitGate gate;
    REQUIRE (gate.onQuitRequested (false, 1000.0) == QuitGate::Decision::StartWaiting);
    REQUIRE (gate.isPending());

    REQUIRE_FALSE (gate.onPoll (false));
    REQUIRE (gate.isPending());

    REQUIRE (gate.onPoll (true));
    REQUIRE_FALSE (gate.isPending());
}

TEST_CASE (QuitGate_ASecondQuitWhileTheCameraIsFinishingIsIgnored)
{
    // The bug: a second Cmd-Q a moment later destroyed the camera mid-write
    // and truncated the movie the first quit was waiting for.
    QuitGate gate;
    REQUIRE (gate.onQuitRequested (false, 1000.0) == QuitGate::Decision::StartWaiting);
    REQUIRE (gate.onQuitRequested (false, 1500.0) == QuitGate::Decision::Ignore);
    REQUIRE (gate.onQuitRequested (false, 1000.0 + QuitGate::kDefaultBoundMs - 1.0) == QuitGate::Decision::Ignore);

    // Still waiting, and it still quits by itself when the files finish.
    REQUIRE (gate.isPending());
    REQUIRE (gate.onPoll (true));
}

TEST_CASE (QuitGate_TheBoundCoversTheCameraOwnFinalizationTimeout)
{
    // CameraController gives up on a movie after 15 s; forcing any sooner
    // cuts off a write the camera side would still have waited for.
    REQUIRE (QuitGate::kDefaultBoundMs > 15000.0);
}

TEST_CASE (QuitGate_ARepeatAfterTheBoundIsTheEscapeHatch)
{
    QuitGate gate (5000.0);
    REQUIRE (gate.onQuitRequested (false, 1000.0) == QuitGate::Decision::StartWaiting);
    REQUIRE (gate.onQuitRequested (false, 3000.0) == QuitGate::Decision::Ignore);
    REQUIRE (gate.onQuitRequested (false, 6000.0) == QuitGate::Decision::ForceQuit);
}

TEST_CASE (QuitGate_ARepeatAfterTheFilesFinishedQuitsNormally)
{
    // A repeat that arrives between the files finishing and the next poll
    // is simply a quit with nothing left to wait for.
    QuitGate gate;
    REQUIRE (gate.onQuitRequested (false, 0.0) == QuitGate::Decision::StartWaiting);
    REQUIRE (gate.onQuitRequested (true, 100.0) == QuitGate::Decision::QuitNow);
    REQUIRE_FALSE (gate.isPending());
}
