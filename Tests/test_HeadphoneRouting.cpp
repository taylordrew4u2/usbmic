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
