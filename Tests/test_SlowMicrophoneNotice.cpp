#include "TestFramework.h"
#include "Core/SlowMicrophoneNotice.h"

using namespace mma;

TEST_CASE (SlowMicrophoneNotice_ARigOfLikeDevicesNamesNoOne)
{
    REQUIRE (! SlowMicrophoneNotice::isFarBehind (5.3, 5.3));
    REQUIRE (! SlowMicrophoneNotice::isFarBehind (5.3, 6.3));
}

TEST_CASE (SlowMicrophoneNotice_OneInterfaceHeldAtALargerBufferIsNamed)
{
    // Another app holds one interface at 2048 frames on a 64-frame rig: 40 ms
    // late in its own channel while the headline reads 5.
    REQUIRE (SlowMicrophoneNotice::isFarBehind (5.3, 46.7));
}

TEST_CASE (SlowMicrophoneNotice_OnePastTheCeilingWhileTheRestAreWithinItIsNamed)
{
    REQUIRE (SlowMicrophoneNotice::isFarBehind (9.5, 10.6));

    // Not when it is past the ceiling by less than a different path makes.
    REQUIRE (! SlowMicrophoneNotice::isFarBehind (9.6, 10.4));

    // Nor, however far behind, while it is within the ceiling itself.
    REQUIRE (! SlowMicrophoneNotice::isFarBehind (2.0, 9.0));
}

TEST_CASE (SlowMicrophoneNotice_ABufferStepThatTakesTheWholeRigPastTheCeilingNamesNoOne)
{
    // At 256 frames every path is about 27 ms, and an interface reporting 48
    // frames more latency is one millisecond behind. The old rule (slowest
    // past 10 ms and a millisecond behind) told the user it ran at a larger
    // buffer than the others and to close apps and replug it.
    REQUIRE (! SlowMicrophoneNotice::isFarBehind (26.7, 27.7));
    REQUIRE (! SlowMicrophoneNotice::isFarBehind (13.3, 17.0));

    // One several milliseconds behind them is still named.
    REQUIRE (SlowMicrophoneNotice::isFarBehind (26.7, 31.7));
    REQUIRE (SlowMicrophoneNotice::isFarBehind (26.7, 64.0));
}
