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

TEST_CASE (DeadChannelDetector_ASilentChannelIsAtTheMeteringFloorNotBelowIt)
{
    // The case the detector exists for, at the level PRODUCTION actually
    // produces. Every other test here feeds -70 dBFS, which the real metering
    // can never emit: it floors at Metering::kMinDb, so a channel with no
    // signal at all arrives as exactly -60.0. With a strict "<" against a
    // -60.0 threshold that channel was never once counted, and the silent-mic
    // warning could not fire for a silent mic.
    //
    // Fed from the constant rather than a literal, so a test can never again
    // pass on a level the pipeline cannot deliver.
    DeadChannelDetector detector (2);

    const float silent = Metering::kMinDb;   // what a dead channel really reads
    const float loud   = -8.0f;

    for (int i = 0; i < 250; ++i)            // 25 s, past the 20 s sustain
        detector.processBlock ({ silent, loud }, 0.1);

    REQUIRE (detector.isChannelDead (0));
    REQUIRE_FALSE (detector.isChannelDead (1));
}
