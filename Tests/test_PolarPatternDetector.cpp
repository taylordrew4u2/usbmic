#include "TestFramework.h"
#include "Core/PolarPatternDetector.h"

using namespace mma;

TEST_CASE (PolarPatternDetector_TriggersAfterSustainedCorrelationWithQuietThirdChannel)
{
    PolarPatternDetector detector;
    bool triggered = false;
    for (int i = 0; i < 110; ++i) // 11s at 0.1s blocks
        triggered = detector.processBlock (0.8f, -50.0f, 0.1);

    REQUIRE (triggered);
    REQUIRE (detector.isTriggered());
}

TEST_CASE (PolarPatternDetector_DoesNotTriggerBelowCorrelationThreshold)
{
    PolarPatternDetector detector;
    bool triggered = false;
    for (int i = 0; i < 110; ++i)
        triggered = detector.processBlock (0.4f, -50.0f, 0.1);

    REQUIRE_FALSE (triggered);
}

TEST_CASE (PolarPatternDetector_DoesNotTriggerWhenThirdChannelIsLoud)
{
    PolarPatternDetector detector;
    bool triggered = false;
    for (int i = 0; i < 110; ++i)
        triggered = detector.processBlock (0.8f, -20.0f, 0.1);

    REQUIRE_FALSE (triggered);
}

TEST_CASE (PolarPatternDetector_ResetsSustainCounterWhenConditionDrops)
{
    PolarPatternDetector detector;
    for (int i = 0; i < 90; ++i) // 9s, not yet triggered
        detector.processBlock (0.8f, -50.0f, 0.1);
    REQUIRE_FALSE (detector.isTriggered());

    detector.processBlock (0.1f, -50.0f, 0.1); // condition drops, resets sustain timer
    for (int i = 0; i < 15; ++i) // only 1.5s more, not enough alone
        detector.processBlock (0.8f, -50.0f, 0.1);

    REQUIRE_FALSE (detector.isTriggered());
}

TEST_CASE (PolarPatternDetector_ResetClearsLatch)
{
    PolarPatternDetector detector;
    for (int i = 0; i < 110; ++i)
        detector.processBlock (0.8f, -50.0f, 0.1);
    REQUIRE (detector.isTriggered());
    detector.reset();
    REQUIRE_FALSE (detector.isTriggered());
}
