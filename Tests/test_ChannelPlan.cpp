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

TEST_CASE (ChannelPlan_ASwitchedOffInputIsNotRecordedAndTheRestKeepTheirNumbers)
{
    // An eight-input interface with two people on inputs 1 and 3. Recording
    // all eight wrote six files of silence and quartered the remaining-time
    // estimate. The unused sockets are switched off; the used ones keep the
    // number printed beside them on the box.
    auto d = iface ("if", "Scarlett 18i8", 4);
    d.disabledInputs = { 1, 3 };

    const auto plan = planChannels ({ d });

    REQUIRE (plan.size() == 2);
    REQUIRE (plan[0].deviceChannel == 0);
    REQUIRE (plan[0].displayName == "Scarlett 18i8 1");
    REQUIRE (plan[1].deviceChannel == 2);
    REQUIRE (plan[1].displayName == "Scarlett 18i8 3");
}

TEST_CASE (ChannelPlan_ANamedInputIsCalledByThatName)
{
    // On an interface each socket is a person. "Scarlett 2i2 2" is not who
    // they are; "Sam" is, and a name needs no socket number after it.
    auto d = iface ("if", "Scarlett 2i2", 2);
    d.inputNames[1] = "Sam";

    const auto plan = planChannels ({ d });

    REQUIRE (plan.size() == 2);
    REQUIRE (plan[0].displayName == "Scarlett 2i2 1");
    REQUIRE (plan[1].displayName == "Sam");
}

TEST_CASE (ChannelPlan_AnInputNameBeatsTheBoxName)
{
    // Naming the box AND an input: the input's own name wins for that socket,
    // and the box name carries the rest.
    auto d = iface ("if", "Scarlett 2i2", 2);
    d.assignedName = "Kitchen";
    d.inputNames[0] = "Alex";

    const auto plan = planChannels ({ d });

    REQUIRE (plan[0].displayName == "Alex");
    REQUIRE (plan[1].displayName == "Kitchen 2");
}
