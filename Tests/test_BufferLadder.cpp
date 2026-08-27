#include "TestFramework.h"
#include "Core/BufferLadder.h"

using namespace mma;

TEST_CASE (BufferLadder_StartsAtSixtyFour)
{
    BufferLadder ladder;
    REQUIRE (ladder.getCurrentSize() == 64);
}

TEST_CASE (BufferLadder_TwoOverrunsDoNotStepUp)
{
    BufferLadder ladder;
    REQUIRE_FALSE (ladder.noteOverrun (1.0));
    REQUIRE_FALSE (ladder.noteOverrun (2.0));
    REQUIRE (ladder.getCurrentSize() == 64);
}

TEST_CASE (BufferLadder_ThreeOverrunsInWindowStepUp)
{
    BufferLadder ladder;
    ladder.noteOverrun (1.0);
    ladder.noteOverrun (2.0);
    REQUIRE (ladder.noteOverrun (3.0));
    REQUIRE (ladder.getCurrentSize() == 128);
}

TEST_CASE (BufferLadder_OverrunsOutsideTheWindowDoNotAccumulate)
{
    BufferLadder ladder;
    ladder.noteOverrun (0.0);
    ladder.noteOverrun (1.0);

    // The first two have aged out of the 30-second window by now, so this is
    // the only one inside it.
    REQUIRE_FALSE (ladder.noteOverrun (100.0));
    REQUIRE (ladder.getCurrentSize() == 64);
}

TEST_CASE (BufferLadder_ClimbsTheWholeLadder)
{
    BufferLadder ladder;
    double t = 0.0;

    auto threeOverruns = [&ladder, &t] { ladder.noteOverrun (t += 1.0); ladder.noteOverrun (t += 1.0); return ladder.noteOverrun (t += 1.0); };

    REQUIRE (threeOverruns());
    REQUIRE (ladder.getCurrentSize() == 128);
    REQUIRE (threeOverruns());
    REQUIRE (ladder.getCurrentSize() == 256);
    REQUIRE (threeOverruns());
    REQUIRE (ladder.getCurrentSize() == 512);
}

TEST_CASE (BufferLadder_StopsAtFiveTwelve)
{
    BufferLadder ladder;
    double t = 0.0;

    for (int i = 0; i < 30; ++i)
        ladder.noteOverrun (t += 0.1);

    REQUIRE (ladder.getCurrentSize() == 512);
    REQUIRE (ladder.isAtMaximum());
    // Past the top of the ladder there is nothing left to do but keep running.
    REQUIRE_FALSE (ladder.noteOverrun (t += 0.1));
}

TEST_CASE (BufferLadder_WindowRestartsAfterAStepUp)
{
    BufferLadder ladder;
    ladder.noteOverrun (1.0);
    ladder.noteOverrun (2.0);
    REQUIRE (ladder.noteOverrun (3.0));

    // Overruns at 64 say nothing about whether 128 is big enough, so the next
    // step needs three fresh ones.
    REQUIRE_FALSE (ladder.noteOverrun (4.0));
    REQUIRE_FALSE (ladder.noteOverrun (5.0));
    REQUIRE (ladder.noteOverrun (6.0));
    REQUIRE (ladder.getCurrentSize() == 256);
}

TEST_CASE (BufferLadder_LogsEveryStep)
{
    BufferLadder ladder;
    ladder.noteOverrun (1.0);
    ladder.noteOverrun (2.0);
    ladder.noteOverrun (3.0);

    const auto& log = ladder.getChangeLog();
    REQUIRE (log.size() == 1);
    REQUIRE (log[0].fromSamples == 64);
    REQUIRE (log[0].toSamples == 128);
    REQUIRE_NEAR (log[0].atSeconds, 3.0, 1e-9);
}

TEST_CASE (BufferLadder_NeverStepsDownDuringARecording)
{
    BufferLadder ladder;
    ladder.noteOverrun (1.0);
    ladder.noteOverrun (2.0);
    ladder.noteOverrun (3.0);
    REQUIRE (ladder.getCurrentSize() == 128);

    ladder.setRecording (true);
    // A buffer change mid-take is a dropout risk (§5.4).
    REQUIRE_FALSE (ladder.resetToLowest());
    REQUIRE (ladder.getCurrentSize() == 128);
}

TEST_CASE (BufferLadder_ResetsWhenNotRecording)
{
    BufferLadder ladder;
    ladder.noteOverrun (1.0);
    ladder.noteOverrun (2.0);
    ladder.noteOverrun (3.0);

    ladder.setRecording (false);
    REQUIRE (ladder.resetToLowest());
    REQUIRE (ladder.getCurrentSize() == 64);
}

TEST_CASE (BufferLadder_StepsUpEvenWhileRecording)
{
    // Stepping up mid-take is allowed -- it is stepping *down* that §5.4
    // forbids. Continuing to drop samples would be worse.
    BufferLadder ladder;
    ladder.setRecording (true);

    ladder.noteOverrun (1.0);
    ladder.noteOverrun (2.0);
    REQUIRE (ladder.noteOverrun (3.0));
    REQUIRE (ladder.getCurrentSize() == 128);
}
