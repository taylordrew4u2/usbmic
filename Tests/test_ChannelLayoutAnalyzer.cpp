#include "TestFramework.h"
#include "Core/ChannelLayoutAnalyzer.h"

using namespace mma;

TEST_CASE (ChannelLayoutAnalyzer_CollapsesToMonoWhenRightChannelSilent)
{
    ChannelLayoutAnalyzer analyzer (48000.0);
    // 3 seconds of signal above -50dBFS on left, right stays below -80dBFS.
    for (int i = 0; i < 300; ++i)
        analyzer.processBlock (-20.0f, -90.0f, 0.0f, 0.0f, 0.01);

    REQUIRE (analyzer.getDecision() == ChannelLayoutDecision::Mono);
}

TEST_CASE (ChannelLayoutAnalyzer_CollapsesToMonoWhenHighlyCorrelatedAndMatched)
{
    ChannelLayoutAnalyzer analyzer (48000.0);
    for (int i = 0; i < 300; ++i)
        analyzer.processBlock (-20.0f, -20.1f, 0.995f, 0.1f, 0.01);

    REQUIRE (analyzer.getDecision() == ChannelLayoutDecision::Mono);
}

TEST_CASE (ChannelLayoutAnalyzer_KeepsStereoWhenDissimilar)
{
    ChannelLayoutAnalyzer analyzer (48000.0);
    for (int i = 0; i < 300; ++i)
        analyzer.processBlock (-20.0f, -25.0f, 0.2f, 3.0f, 0.01);

    REQUIRE (analyzer.getDecision() == ChannelLayoutDecision::Stereo);
}

TEST_CASE (ChannelLayoutAnalyzer_DefaultsToMonoOnTimeoutWithNoSignal)
{
    ChannelLayoutAnalyzer analyzer (48000.0);
    // Never crosses -50dBFS trigger; after 60s, defaults to mono.
    for (int i = 0; i < 6100; ++i)
        analyzer.processBlock (-70.0f, -70.0f, 0.0f, 0.0f, 0.01);

    REQUIRE (analyzer.getDecision() == ChannelLayoutDecision::Mono);
}

TEST_CASE (ChannelLayoutAnalyzer_PendingBeforeWindowCompletes)
{
    ChannelLayoutAnalyzer analyzer (48000.0);
    analyzer.processBlock (-20.0f, -20.0f, 0.995f, 0.1f, 0.5);
    REQUIRE (analyzer.getDecision() == ChannelLayoutDecision::Pending);
    REQUIRE (analyzer.isWindowActive());
}

TEST_CASE (ChannelLayoutAnalyzer_TakesTheRightChannelWhenTheLeftIsTheSilentOne)
{
    // §2.1's first condition is "one channel stays below -80 dBFS" and does not
    // say which. A microphone wired to the right presents exactly like one
    // wired to the left, so the side has to be answered rather than assumed --
    // assuming channel 0 is how such a device records silence.
    ChannelLayoutAnalyzer analyzer (48000.0);

    // Left digitally silent, right carrying signal.
    for (int i = 0; i < 300; ++i)
        analyzer.processBlock (-200.0f, -12.0f, 0.0f, 100.0f, 0.01);

    REQUIRE (analyzer.getDecision() == ChannelLayoutDecision::Mono);
    REQUIRE (analyzer.getMonoSourceChannel() == 1);
}

TEST_CASE (ChannelLayoutAnalyzer_TakesTheLeftChannelForEveryOtherLayout)
{
    // The side only ever moves for the case above. A duplicated source, a
    // right-silent device and ordinary stereo all read from channel 0, so this
    // cannot start rerouting microphones that were working.
    {
        ChannelLayoutAnalyzer duplicated (48000.0);
        for (int i = 0; i < 300; ++i)
            duplicated.processBlock (-12.0f, -12.0f, 0.999f, 0.0f, 0.01);

        REQUIRE (duplicated.getDecision() == ChannelLayoutDecision::Mono);
        REQUIRE (duplicated.getMonoSourceChannel() == 0);
    }

    {
        ChannelLayoutAnalyzer rightSilent (48000.0);
        for (int i = 0; i < 300; ++i)
            rightSilent.processBlock (-12.0f, -200.0f, 0.0f, 100.0f, 0.01);

        REQUIRE (rightSilent.getDecision() == ChannelLayoutDecision::Mono);
        REQUIRE (rightSilent.getMonoSourceChannel() == 0);
    }

    {
        ChannelLayoutAnalyzer stereo (48000.0);
        for (int i = 0; i < 300; ++i)
            stereo.processBlock (-12.0f, -18.0f, 0.20f, 6.0f, 0.01);

        REQUIRE (stereo.getDecision() == ChannelLayoutDecision::Stereo);
        REQUIRE (stereo.getMonoSourceChannel() == 0);
    }
}

TEST_CASE (ChannelLayoutAnalyzer_AnswersTheSideBeforeItHasDecided)
{
    // The verdict takes three seconds of signal, and up to sixty if the room
    // stays quiet. A right-wired microphone must not record silence for that
    // long, so the side is answerable from the first block that carries audio.
    ChannelLayoutAnalyzer analyzer (48000.0);

    analyzer.processBlock (-200.0f, -10.0f, 0.0f, 100.0f, 0.0013);

    REQUIRE (analyzer.getDecision() == ChannelLayoutDecision::Pending);
    REQUIRE (analyzer.getMonoSourceChannel() == 1);
}

TEST_CASE (ChannelLayoutAnalyzer_SilenceOnBothSidesStaysOnTheLeft)
{
    // Nothing has been heard from either side, so there is no evidence to move
    // on. Guessing the right here would reroute every microphone in a quiet
    // room away from the channel it is probably using.
    ChannelLayoutAnalyzer analyzer (48000.0);

    for (int i = 0; i < 50; ++i)
        analyzer.processBlock (-200.0f, -200.0f, 0.0f, 0.0f, 0.01);

    REQUIRE (analyzer.getMonoSourceChannel() == 0);
}
