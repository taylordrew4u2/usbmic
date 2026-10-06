#include "TestFramework.h"
#include "Core/HeadphoneRouting.h"

using namespace mma;

TEST_CASE (HeadphoneRouting_EveryJackGetsTheMixByDefault)
{
    const std::vector<CombinedDeviceOutputs> layout { { "yeti-a", 2 }, { "yeti-b", 2 } };
    const auto gains = headphoneChannelGains (layout, nullptr);

    REQUIRE (gains.size() == 4u);
    for (float g : gains)
        REQUIRE (g == 1.0f);
}

TEST_CASE (HeadphoneRouting_SwitchingOnePersonOffSilencesOnlyTheirJack)
{
    const std::vector<CombinedDeviceOutputs> layout { { "yeti-a", 2 }, { "yeti-b", 2 }, { "yeti-c", 2 } };
    const auto gains = headphoneChannelGains (layout, [] (const std::string& id) { return id != "yeti-b"; });

    REQUIRE (gains.size() == 6u);
    REQUIRE (gains[0] == 1.0f);
    REQUIRE (gains[1] == 1.0f);
    REQUIRE (gains[2] == 0.0f);
    REQUIRE (gains[3] == 0.0f);
    REQUIRE (gains[4] == 1.0f);
    REQUIRE (gains[5] == 1.0f);
}

TEST_CASE (HeadphoneRouting_AMicWithNoJackTakesNoChannels)
{
    // A lavalier with no output sits in the combined device but brings no
    // output channels; the next mic's jack must not shift onto its slot.
    const std::vector<CombinedDeviceOutputs> layout { { "lav", 0 }, { "yeti", 2 } };
    const auto gains = headphoneChannelGains (layout, [] (const std::string& id) { return id != "lav"; });

    REQUIRE (gains.size() == 2u);
    REQUIRE (gains[0] == 1.0f);
    REQUIRE (gains[1] == 1.0f);
    REQUIRE (combinedDeviceHasHeadphones (layout));
    REQUIRE_FALSE (combinedDeviceHasHeadphones ({ { "lav", 0 } }));
}

TEST_CASE (HeadphoneRouting_LayoutIsCompleteOnlyWhenEverySubDeviceIsActive)
{
    REQUIRE (combinedLayoutIsComplete ({ { "yeti-b", 2 }, { "yeti-a", 2 } }, { "yeti-a", "yeti-b" }));
    REQUIRE (combinedLayoutIsComplete ({ { "lav", 0 } }, { "lav" }));
    REQUIRE_FALSE (combinedLayoutIsComplete ({}, { "yeti-a" }));
    REQUIRE_FALSE (combinedLayoutIsComplete ({ { "yeti-a", 2 } }, { "yeti-a", "yeti-b" }));
}

TEST_CASE (HeadphoneRouting_ANewCombinedDeviceIsWaitedForUntilItsJacksAppear)
{
    // Empty for the first few reads after creation, as the HAL builds it.
    int reads = 0, pauses = 0;
    const auto read = [&reads]
    {
        return ++reads <= 3 ? std::vector<CombinedDeviceOutputs> {}
                            : std::vector<CombinedDeviceOutputs> { { "yeti-a", 2 }, { "yeti-b", 2 } };
    };

    REQUIRE (waitForCombinedLayout (read, { "yeti-a", "yeti-b" }, 50, [&pauses] { ++pauses; }));
    REQUIRE (reads == 4);
    REQUIRE (pauses == 3);
}

TEST_CASE (HeadphoneRouting_ASubDeviceThatNeverActivatesCostsOnlyTheBound)
{
    int reads = 0, pauses = 0;
    const auto read = [&reads]
    {
        ++reads;
        return std::vector<CombinedDeviceOutputs> { { "yeti-a", 2 } };
    };

    REQUIRE_FALSE (waitForCombinedLayout (read, { "yeti-a", "gone" }, 5, [&pauses] { ++pauses; }));
    REQUIRE (pauses == 5);
    REQUIRE (reads == 6);

    // Already there: no pause at all.
    pauses = 0;
    REQUIRE (waitForCombinedLayout (read, { "yeti-a" }, 5, [&pauses] { ++pauses; }));
    REQUIRE (pauses == 0);
}
