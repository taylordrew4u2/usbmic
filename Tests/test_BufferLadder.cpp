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

TEST_CASE (BufferLadder_ATakeRecordsOnlyItsOwnStepsTimedFromItsStart)
{
    BufferLadder ladder;

    // An earlier take (or idle time) stepped the ladder at 100 s of uptime.
    ladder.noteOverrun (98.0);
    ladder.noteOverrun (99.0);
    REQUIRE (ladder.noteOverrun (100.0));

    // This take starts at 500 s of uptime and steps 20 s in.
    const auto startIndex = ladder.getChangeLog().size();
    const double takeStart = 500.0;
    ladder.noteOverrun (518.0);
    ladder.noteOverrun (519.0);
    REQUIRE (ladder.noteOverrun (520.0));

    const auto changes = selectTakeBufferChanges (ladder.getChangeLog(), startIndex, takeStart);
    REQUIRE (changes.size() == 1);
    REQUIRE (changes[0].fromSamples == 128);
    REQUIRE (changes[0].toSamples == 256);
    REQUIRE (changes[0].atSeconds > 19.99 && changes[0].atSeconds < 20.01);
}

TEST_CASE (BufferLadder_TakeStepsAreNeverBeforeZeroAndABadIndexIsEmpty)
{
    const std::vector<BufferSizeChange> log { { 499.5, 64, 128 } };

    // A step logged a hair before the take's audio t=0 still belongs to it.
    const auto changes = selectTakeBufferChanges (log, 0, 500.0);
    REQUIRE (changes.size() == 1);
    REQUIRE (changes[0].atSeconds == 0.0);

    REQUIRE (selectTakeBufferChanges (log, 5, 500.0).empty());
}
