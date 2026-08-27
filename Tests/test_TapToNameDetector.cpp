#include "TestFramework.h"
#include "Core/TapToNameDetector.h"

using namespace mma;

namespace {
constexpr double kBlock = 0.010; // 10 ms blocks
}

TEST_CASE (TapToName_StartsListening)
{
    TapToNameDetector d (3);
    REQUIRE (d.getResult() == TapResult::Listening);
    REQUIRE (d.getTappedChannel() == -1);
}

TEST_CASE (TapToName_IdentifiesASingleTappedChannel)
{
    TapToNameDetector d (3);

    // Channel 1 loud, others silent, held past 300 ms.
    for (int i = 0; i < 40; ++i)
        d.processBlock ({ -80.0f, -10.0f, -80.0f }, kBlock);

    REQUIRE (d.getResult() == TapResult::ChannelIdentified);
    REQUIRE (d.getTappedChannel() == 1);
}

TEST_CASE (TapToName_ShortTapIsNotEnough)
{
    TapToNameDetector d (2);

    // 100 ms, well short of the 300 ms §14.6 requires.
    for (int i = 0; i < 10; ++i)
        d.processBlock ({ -10.0f, -80.0f }, kBlock);

    REQUIRE (d.getResult() == TapResult::Listening);
}

TEST_CASE (TapToName_TwoLoudChannelsAreAmbiguous)
{
    TapToNameDetector d (3);

    // §14.6: guessing would name the wrong skull, so it must refuse.
    const auto r = d.processBlock ({ -10.0f, -12.0f, -80.0f }, kBlock);

    REQUIRE (r == TapResult::Ambiguous);
    REQUIRE (d.getTappedChannel() == -1);
}

TEST_CASE (TapToName_ANoisyNeighbourBlocksIdentification)
{
    TapToNameDetector d (2);

    // Channel 0 is tapped, but channel 1 is above the quiet threshold, so the
    // tap cannot be attributed cleanly.
    for (int i = 0; i < 40; ++i)
        d.processBlock ({ -10.0f, -30.0f }, kBlock);

    REQUIRE (d.getResult() == TapResult::Listening);
}

TEST_CASE (TapToName_ExactlyAtQuietThresholdIsNotQuietEnough)
{
    TapToNameDetector d (2);

    // §14.6 says others stay *below* -45, so -45 itself does not qualify.
    for (int i = 0; i < 40; ++i)
        d.processBlock ({ -10.0f, TapToNameDetector::kQuietThresholdDb }, kBlock);

    REQUIRE (d.getResult() == TapResult::Listening);
}

TEST_CASE (TapToName_ExactlyAtTapThresholdIsNotLoudEnough)
{
    TapToNameDetector d (2);

    // §14.6 says exceeds -25, so -25 itself does not count as a tap.
    for (int i = 0; i < 40; ++i)
        d.processBlock ({ TapToNameDetector::kTapThresholdDb, -80.0f }, kBlock);

    REQUIRE (d.getResult() == TapResult::Listening);
}

TEST_CASE (TapToName_InterruptedTapRestartsTheClock)
{
    TapToNameDetector d (2);

    for (int i = 0; i < 20; ++i)
        d.processBlock ({ -10.0f, -80.0f }, kBlock);

    // Silence in the middle: §14.6's 300 ms must be continuous.
    d.processBlock ({ -80.0f, -80.0f }, kBlock);

    // Only 200 ms since the interruption, so the earlier 200 ms must not carry
    // over and complete it.
    for (int i = 0; i < 20; ++i)
        d.processBlock ({ -10.0f, -80.0f }, kBlock);
    REQUIRE (d.getResult() == TapResult::Listening);

    // A further 100 ms takes this run past 300 ms on its own.
    for (int i = 0; i < 11; ++i)
        d.processBlock ({ -10.0f, -80.0f }, kBlock);
    REQUIRE (d.getResult() == TapResult::ChannelIdentified);
}

TEST_CASE (TapToName_ResultLatchesUntilReset)
{
    TapToNameDetector d (2);

    for (int i = 0; i < 40; ++i)
        d.processBlock ({ -10.0f, -80.0f }, kBlock);
    REQUIRE (d.getResult() == TapResult::ChannelIdentified);

    // Silence afterwards must not undo it while the user types a name.
    for (int i = 0; i < 40; ++i)
        d.processBlock ({ -80.0f, -80.0f }, kBlock);
    REQUIRE (d.getResult() == TapResult::ChannelIdentified);
    REQUIRE (d.getTappedChannel() == 0);
}

TEST_CASE (TapToName_ResetListensForTheNextMic)
{
    TapToNameDetector d (2);

    for (int i = 0; i < 40; ++i)
        d.processBlock ({ -10.0f, -80.0f }, kBlock);
    REQUIRE (d.getTappedChannel() == 0);

    d.reset();
    REQUIRE (d.getResult() == TapResult::Listening);

    for (int i = 0; i < 40; ++i)
        d.processBlock ({ -80.0f, -10.0f }, kBlock);
    REQUIRE (d.getTappedChannel() == 1);
}

TEST_CASE (TapToName_ResetClearsAnAmbiguousResult)
{
    TapToNameDetector d (2);

    d.processBlock ({ -10.0f, -10.0f }, kBlock);
    REQUIRE (d.getResult() == TapResult::Ambiguous);

    d.reset();
    REQUIRE (d.getResult() == TapResult::Listening);
}

TEST_CASE (TapToName_MismatchedChannelCountIsIgnored)
{
    TapToNameDetector d (3);
    // A hot-plug mid-flow must not be read as a tap on the wrong mic.
    for (int i = 0; i < 40; ++i)
        d.processBlock ({ -10.0f, -80.0f }, kBlock);

    REQUIRE (d.getResult() == TapResult::Listening);
}
