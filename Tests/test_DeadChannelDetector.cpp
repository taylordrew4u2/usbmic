#include "TestFramework.h"
#include "Core/DeadChannelDetector.h"

using namespace mma;

TEST_CASE (DeadChannelDetector_FlagsChannelSilentWhileOthersActive)
{
    DeadChannelDetector detector (2);
    for (int i = 0; i < 210; ++i) // 21s at 0.1s blocks
        detector.processBlock ({ -70.0f, -20.0f }, 0.1);

    REQUIRE (detector.isChannelDead (0));
    REQUIRE_FALSE (detector.isChannelDead (1));
}

TEST_CASE (DeadChannelDetector_DoesNotFlagWhenAllChannelsSilent)
{
    DeadChannelDetector detector (2);
    for (int i = 0; i < 210; ++i)
        detector.processBlock ({ -70.0f, -70.0f }, 0.1);

    REQUIRE_FALSE (detector.isChannelDead (0));
    REQUIRE_FALSE (detector.isChannelDead (1));
}

TEST_CASE (DeadChannelDetector_RequiresFull20SecondsSustained)
{
    DeadChannelDetector detector (2);
    for (int i = 0; i < 150; ++i) // only 15s
        detector.processBlock ({ -70.0f, -20.0f }, 0.1);

    REQUIRE_FALSE (detector.isChannelDead (0));
}

TEST_CASE (DeadChannelDetector_ClearsWhenChannelBecomesActiveAgain)
{
    DeadChannelDetector detector (2);
    for (int i = 0; i < 210; ++i)
        detector.processBlock ({ -70.0f, -20.0f }, 0.1);
    REQUIRE (detector.isChannelDead (0));

    detector.processBlock ({ -20.0f, -20.0f }, 0.1);
    REQUIRE_FALSE (detector.isChannelDead (0));
}
