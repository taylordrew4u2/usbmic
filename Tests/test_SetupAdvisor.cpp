#include "TestFramework.h"
#include "Core/SetupAdvisor.h"

using namespace mma;

namespace {

bool hasIssue (const std::vector<SetupAdvice>& advice, SetupIssue issue)
{
    for (const auto& a : advice)
        if (a.issue == issue)
            return true;
    return false;
}

const SetupAdvice* find (const std::vector<SetupAdvice>& advice, SetupIssue issue)
{
    for (const auto& a : advice)
        if (a.issue == issue)
            return &a;
    return nullptr;
}

} // namespace

TEST_CASE (SetupAdvisor_QuietWhenNothingIsWrong)
{
    SetupAdvisor advisor;
    REQUIRE (advisor.getActiveAdvice (0.0).empty());
}

TEST_CASE (SetupAdvisor_ReportsBusPowerExhaustion)
{
    SetupAdvisor advisor;
    // §14.2: two or more drops inside five minutes with three or more mics.
    advisor.noteDeviceDropout (10.0, 4);
    advisor.noteDeviceDropout (20.0, 4);

    REQUIRE (hasIssue (advisor.getActiveAdvice (30.0), SetupIssue::BusPowerExhausted));
}

TEST_CASE (SetupAdvisor_BusPowerMessageNamesThePoweredHub)
{
    SetupAdvisor advisor;
    advisor.noteDeviceDropout (10.0, 4);
    advisor.noteDeviceDropout (20.0, 4);

    const auto advice = advisor.getActiveAdvice (30.0);
    const auto* a = find (advice, SetupIssue::BusPowerExhausted);
    REQUIRE (a != nullptr);
    // §14.2 insists on "its own power adapter" wording: a hub that only plugs
    // into the computer adds ports, not power, and the two look identical.
    REQUIRE (a->message.find ("own power adapter") != std::string::npos);
}

TEST_CASE (SetupAdvisor_SingleDropoutIsNotEnough)
{
    SetupAdvisor advisor;
    advisor.noteDeviceDropout (10.0, 4);
    REQUIRE_FALSE (hasIssue (advisor.getActiveAdvice (20.0), SetupIssue::BusPowerExhausted));
}

TEST_CASE (SetupAdvisor_ReportsControllerContention)
{
    SetupAdvisor advisor;

    std::vector<ControllerContentionDetector::DeviceControllerInfo> devices;
    devices.push_back ({ "reader", "usb-controller-0", true, false });
    devices.push_back ({ "yeti", "usb-controller-0", false, true });

    advisor.updateControllerTopology (devices);
    REQUIRE (hasIssue (advisor.getActiveAdvice (0.0), SetupIssue::ControllerContention));
}

TEST_CASE (SetupAdvisor_NoContentionOnSeparateControllers)
{
    SetupAdvisor advisor;

    std::vector<ControllerContentionDetector::DeviceControllerInfo> devices;
    devices.push_back ({ "reader", "usb-controller-0", true, false });
    devices.push_back ({ "yeti", "usb-controller-1", false, true });

    advisor.updateControllerTopology (devices);
    REQUIRE_FALSE (hasIssue (advisor.getActiveAdvice (0.0), SetupIssue::ControllerContention));
}

TEST_CASE (SetupAdvisor_ContentionClearsWhenTopologyChanges)
{
    SetupAdvisor advisor;

    std::vector<ControllerContentionDetector::DeviceControllerInfo> shared;
    shared.push_back ({ "reader", "c0", true, false });
    shared.push_back ({ "yeti", "c0", false, true });
    advisor.updateControllerTopology (shared);
    REQUIRE (hasIssue (advisor.getActiveAdvice (0.0), SetupIssue::ControllerContention));

    // Reader moved to the built-in slot: the warning must go away rather than
    // stick around telling the user to fix something they already fixed.
    std::vector<ControllerContentionDetector::DeviceControllerInfo> separate;
    separate.push_back ({ "yeti", "c0", false, true });
    advisor.updateControllerTopology (separate);
    REQUIRE_FALSE (hasIssue (advisor.getActiveAdvice (0.0), SetupIssue::ControllerContention));
}

TEST_CASE (SetupAdvisor_SilentChannelNamesTheMuteButton)
{
    SetupAdvisor advisor;
    advisor.setChannelNames ({ "Kitchen", "Couch" });

    // Kitchen silent while Couch is live, held long enough to qualify (§8.1).
    for (int i = 0; i < 25; ++i)
        advisor.updateChannelLevels ({ -80.0f, -20.0f }, 1.0);

    const auto advice = advisor.getActiveAdvice (30.0);
    const auto* a = find (advice, SetupIssue::SilentChannel);
    REQUIRE (a != nullptr);
    REQUIRE (a->channelIndex == 0);
    // §10.5: the hardware mute switch is the single most common failure, so it
    // is named rather than hinted at.
    REQUIRE (a->message.find ("mute button") != std::string::npos);
}

TEST_CASE (SetupAdvisor_SilentChannelUsesTheAssignedName)
{
    SetupAdvisor advisor;
    advisor.setChannelNames ({ "Kitchen", "Couch" });

    for (int i = 0; i < 25; ++i)
        advisor.updateChannelLevels ({ -80.0f, -20.0f }, 1.0);

    const auto advice = advisor.getActiveAdvice (30.0);
    const auto* a = find (advice, SetupIssue::SilentChannel);
    REQUIRE (a != nullptr);
    // §6.2/§14.6: a novice cannot act on "channel 1".
    REQUIRE (a->message.find ("Kitchen") != std::string::npos);
}

TEST_CASE (SetupAdvisor_FallsBackToMicNumberWithoutAName)
{
    SetupAdvisor advisor;

    for (int i = 0; i < 25; ++i)
        advisor.updateChannelLevels ({ -80.0f, -20.0f }, 1.0);

    const auto advice = advisor.getActiveAdvice (30.0);
    const auto* a = find (advice, SetupIssue::SilentChannel);
    REQUIRE (a != nullptr);
    REQUIRE (a->message.find ("Mic 1") != std::string::npos);
}

TEST_CASE (SetupAdvisor_SilenceEverywhereIsNotADeadChannel)
{
    SetupAdvisor advisor;

    // Nobody is talking. That is not a fault, and warning about it would train
    // the user to ignore the warning.
    for (int i = 0; i < 25; ++i)
        advisor.updateChannelLevels ({ -80.0f, -80.0f }, 1.0);

    REQUIRE_FALSE (hasIssue (advisor.getActiveAdvice (30.0), SetupIssue::SilentChannel));
}

TEST_CASE (SetupAdvisor_PolarPatternMessageAvoidsTheWordBleed)
{
    SetupAdvisor advisor;

    // §14.4: correlation above 0.6 sustained for 10 s with a third channel quiet.
    for (int i = 0; i < 12; ++i)
        advisor.updatePolarPattern (0.9f, -60.0f, 1.0);

    const auto advice = advisor.getActiveAdvice (0.0);
    const auto* a = find (advice, SetupIssue::NonCardioidPattern);
    REQUIRE (a != nullptr);
    REQUIRE (a->message.find ("bleed") == std::string::npos);
    REQUIRE (a->message.find ("single-heart") != std::string::npos);
}

TEST_CASE (SetupAdvisor_PowerIsReportedBeforeOtherIssues)
{
    SetupAdvisor advisor;

    advisor.noteDeviceDropout (10.0, 4);
    advisor.noteDeviceDropout (20.0, 4);
    for (int i = 0; i < 25; ++i)
        advisor.updateChannelLevels ({ -80.0f, -20.0f }, 1.0);

    const auto advice = advisor.getActiveAdvice (30.0);
    REQUIRE (advice.size() >= 2);
    // Power causes the dropouts everything else gets blamed for, so it leads.
    REQUIRE (advice.front().issue == SetupIssue::BusPowerExhausted);
}

TEST_CASE (SetupAdvisor_ResetClearsAdvice)
{
    SetupAdvisor advisor;

    for (int i = 0; i < 25; ++i)
        advisor.updateChannelLevels ({ -80.0f, -20.0f }, 1.0);
    REQUIRE (hasIssue (advisor.getActiveAdvice (30.0), SetupIssue::SilentChannel));

    advisor.reset();
    REQUIRE_FALSE (hasIssue (advisor.getActiveAdvice (30.0), SetupIssue::SilentChannel));
}
