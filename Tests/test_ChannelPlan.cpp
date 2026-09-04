#include "TestFramework.h"
#include "Core/ChannelPlan.h"

using namespace mma;

static ChannelPlanDevice iface (const std::string& key, const std::string& name, int inputs)
{
    ChannelPlanDevice d;
    d.deviceKey = key;
    d.productName = name;
    d.inputChannelCount = inputs;
    return d;
}

TEST_CASE (ChannelPlan_TwoInputInterfaceIsTwoChannels)
{
    // The reported rig: two people plugged into one small interface. The screen
    // and the files must agree that this is two microphones, before recording
    // starts as well as during it.
    const auto plan = planChannels ({ iface ("if", "Scarlett 2i2", 2) });

    REQUIRE (plan.size() == 2);
    REQUIRE (plan[0].deviceChannel == 0);
    REQUIRE (plan[1].deviceChannel == 1);
}

TEST_CASE (ChannelPlan_InterfaceInputsAreToldApartByNumber)
{
    // Four identical strips over four differently-named files is the bug this
    // prevents: the person watching cannot tell which meter is whose.
    const auto plan = planChannels ({ iface ("if", "Scarlett 18i8", 4) });

    REQUIRE (plan.size() == 4);
    REQUIRE (plan[0].displayName == "Scarlett 18i8 1");
    REQUIRE (plan[1].displayName == "Scarlett 18i8 2");
    REQUIRE (plan[2].displayName == "Scarlett 18i8 3");
    REQUIRE (plan[3].displayName == "Scarlett 18i8 4");
}

TEST_CASE (ChannelPlan_SingleInputMicIsNotNumbered)
{
    const auto plan = planChannels ({ iface ("mic", "Blue Yeti", 1) });

    REQUIRE (plan.size() == 1);
    REQUIRE (plan[0].displayName == "Blue Yeti");
}

TEST_CASE (ChannelPlan_TheNameTheUserGaveThePortWins)
{
    auto d = iface ("if", "Scarlett 2i2", 2);
    d.assignedName = "Kitchen";

    const auto plan = planChannels ({ d });

    REQUIRE (plan[0].displayName == "Kitchen 1");
    REQUIRE (plan[1].displayName == "Kitchen 2");
}

TEST_CASE (ChannelPlan_StereoMicCollapsesOnlyOnceTheAnalyzerHasSaidSo)
{
    auto undecided = iface ("mic", "Stereo USB", 2);
    REQUIRE (planChannels ({ undecided }).size() == 2);

    auto decided = undecided;
    decided.knownDuplicateStereo = true;
    const auto plan = planChannels ({ decided });

    REQUIRE (plan.size() == 1);
    // Collapsed to one channel, so there is nothing to tell apart and no number.
    REQUIRE (plan[0].displayName == "Stereo USB");
}

TEST_CASE (ChannelPlan_ChannelsRunInDeviceOrderAcrossARig)
{
    const auto plan = planChannels ({ iface ("a", "Yeti", 1),
                                      iface ("b", "2i2", 2),
                                      iface ("c", "AT2020", 1) });

    REQUIRE (plan.size() == 4);
    REQUIRE (plan[0].deviceKey == "a");
    REQUIRE (plan[1].deviceKey == "b");
    REQUIRE (plan[1].deviceChannel == 0);
    REQUIRE (plan[2].deviceKey == "b");
    REQUIRE (plan[2].deviceChannel == 1);
    REQUIRE (plan[3].deviceKey == "c");
}
