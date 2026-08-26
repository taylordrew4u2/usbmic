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
