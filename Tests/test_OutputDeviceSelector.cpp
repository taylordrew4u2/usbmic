#include "TestFramework.h"
#include "Core/OutputDeviceSelector.h"

using namespace mma;

namespace {

OutputDeviceCandidate makeDevice (const std::string& id)
{
    OutputDeviceCandidate d;
    d.id = id;
    d.displayName = id;
    return d;
}

} // namespace

TEST_CASE (OutputDeviceSelector_PrefersTheRememberedDeviceWhenPresent)
{
    auto remembered = makeDevice ("remembered");
    auto jack = makeDevice ("jack");
    jack.hasPhysicalHeadphoneJack = true;
    auto fresh = makeDevice ("fresh");
    fresh.appearedAfterLaunch = true;

    auto result = OutputDeviceSelector::select ({ jack, fresh, remembered }, "remembered");

    REQUIRE (result.found);
    REQUIRE (result.id == "remembered");
    REQUIRE (result.displayName == "remembered");
    REQUIRE (result.reason == OutputSelectionReason::RememberedFromPreviousSession);
}

TEST_CASE (OutputDeviceTracker_DoesNotCallTheLaunchSnapshotNew)
{
    OutputDeviceTracker tracker;
    const auto first = tracker.observe ({ makeDevice ("built-in"), makeDevice ("amp") });

    REQUIRE_FALSE (first[0].appearedAfterLaunch);
    REQUIRE_FALSE (first[1].appearedAfterLaunch);
    REQUIRE (first[0].connectionOrder < first[1].connectionOrder);
}

TEST_CASE (OutputDeviceTracker_OnlyCallsAnActualArrivalNew)
{
    OutputDeviceTracker tracker;
    tracker.observe ({ makeDevice ("built-in"), makeDevice ("amp") });

    const auto unchanged = tracker.observe ({ makeDevice ("amp"), makeDevice ("built-in") });
    REQUIRE_FALSE (unchanged[0].appearedAfterLaunch);
    REQUIRE_FALSE (unchanged[1].appearedAfterLaunch);

    const auto withDock = tracker.observe ({ makeDevice ("built-in"), makeDevice ("amp"),
                                             makeDevice ("dock") });
    REQUIRE_FALSE (withDock[0].appearedAfterLaunch);
    REQUIRE_FALSE (withDock[1].appearedAfterLaunch);
    REQUIRE (withDock[2].appearedAfterLaunch);
}

TEST_CASE (OutputDeviceTracker_KeepsAnArrivalNewUntilThatConnectionLeaves)
{
    OutputDeviceTracker tracker;
    tracker.observe ({ makeDevice ("built-in") });

    const auto arrived = tracker.observe ({ makeDevice ("built-in"), makeDevice ("dock") });
    REQUIRE (arrived[1].appearedAfterLaunch);
    const auto arrivalOrder = arrived[1].connectionOrder;

    // An input-device notification can cause an identical output snapshot.
    // The dock is still the most recently connected output and must retain
    // priority two until it is physically removed.
    const auto unchanged = tracker.observe ({ makeDevice ("dock"), makeDevice ("built-in") });
    REQUIRE (unchanged[0].appearedAfterLaunch);
    REQUIRE (unchanged[0].connectionOrder == arrivalOrder);

    const auto selection = OutputDeviceSelector::select (unchanged, {});
    REQUIRE (selection.id == std::string ("dock"));
    REQUIRE (selection.reason == OutputSelectionReason::NewlyConnected);
}

TEST_CASE (OutputDeviceTracker_ReconnectedDeviceIsANewArrival)
{
    OutputDeviceTracker tracker;
    tracker.observe ({ makeDevice ("amp") });
    tracker.observe ({});

    const auto reconnected = tracker.observe ({ makeDevice ("amp") });
    REQUIRE (reconnected[0].appearedAfterLaunch);
}

TEST_CASE (OutputDeviceSelector_FallsPastARememberedDeviceThatIsAbsent)
{
    auto jack = makeDevice ("jack");
    jack.hasPhysicalHeadphoneJack = true;

    auto result = OutputDeviceSelector::select ({ jack }, "not-plugged-in");

    REQUIRE (result.found);
    REQUIRE (result.id == "jack");
    REQUIRE (result.reason == OutputSelectionReason::PhysicalHeadphoneJack);
}

TEST_CASE (OutputDeviceSelector_PrefersANewlyConnectedDeviceOverAHeadphoneJack)
{
    auto jack = makeDevice ("jack");
    jack.hasPhysicalHeadphoneJack = true;
    auto fresh = makeDevice ("fresh");
    fresh.appearedAfterLaunch = true;

    auto result = OutputDeviceSelector::select ({ jack, fresh }, "");

    REQUIRE (result.found);
    REQUIRE (result.id == "fresh");
    REQUIRE (result.reason == OutputSelectionReason::NewlyConnected);
}

TEST_CASE (OutputDeviceSelector_TakesTheMostRecentlyConnectedDevice)
{
    auto first = makeDevice ("first");
    first.appearedAfterLaunch = true;
    first.connectionOrder = 1;
    auto second = makeDevice ("second");
    second.appearedAfterLaunch = true;
    second.connectionOrder = 2;

    auto result = OutputDeviceSelector::select ({ first, second }, "");

    REQUIRE (result.id == "second");
}

TEST_CASE (OutputDeviceSelector_FallsBackToSystemDefault)
{
    auto plain = makeDevice ("plain");
    auto def = makeDevice ("default");
    def.isSystemDefault = true;

    auto result = OutputDeviceSelector::select ({ plain, def }, "");

    REQUIRE (result.found);
    REQUIRE (result.id == "default");
    REQUIRE (result.reason == OutputSelectionReason::SystemDefault);
}

TEST_CASE (OutputDeviceSelector_NeverSelectsAMicrophonePlaybackEndpoint)
{
    // §5.2: the mic jacks carry non-defeatable analog direct monitoring, so they
    // are excluded even when they would otherwise win on every priority.
    auto mic = makeDevice ("yeti-headphone-out");
    mic.isMicrophonePlaybackEndpoint = true;
    mic.hasPhysicalHeadphoneJack = true;
    mic.isSystemDefault = true;
    mic.appearedAfterLaunch = true;

    auto result = OutputDeviceSelector::select ({ mic }, "yeti-headphone-out");

    REQUIRE_FALSE (result.found);
    REQUIRE_FALSE (result.explanation.empty());
}

TEST_CASE (OutputDeviceSelector_NeverRoutesToADeviceThatIsAlsoASelectedInput)
{
    // §5.5: output into an active input is a feedback loop by construction.
    auto loopback = makeDevice ("loopback");
    loopback.isAlsoSelectedInput = true;
    loopback.isSystemDefault = true;

    auto result = OutputDeviceSelector::select ({ loopback }, "");

    REQUIRE_FALSE (result.found);
}

TEST_CASE (OutputDeviceSelector_SkipsIneligibleDevicesButStillPicksAnEligibleOne)
{
    auto mic = makeDevice ("mic");
    mic.isMicrophonePlaybackEndpoint = true;
    mic.appearedAfterLaunch = true;
    mic.connectionOrder = 9;

    auto amp = makeDevice ("amp");
    amp.hasPhysicalHeadphoneJack = true;

    auto result = OutputDeviceSelector::select ({ mic, amp }, "");

    REQUIRE (result.found);
    REQUIRE (result.id == "amp");
}

TEST_CASE (OutputDeviceSelector_ExplainsItselfWhenThereAreNoDevicesAtAll)
{
    auto result = OutputDeviceSelector::select ({}, "");

    REQUIRE_FALSE (result.found);
    REQUIRE_FALSE (result.explanation.empty());
}

TEST_CASE (OutputDeviceSelector_EligibilityRuleIsExplicit)
{
    auto ok = makeDevice ("ok");
    REQUIRE (OutputDeviceSelector::isEligible (ok));

    auto micEndpoint = makeDevice ("mic");
    micEndpoint.isMicrophonePlaybackEndpoint = true;
    REQUIRE_FALSE (OutputDeviceSelector::isEligible (micEndpoint));

    auto alsoInput = makeDevice ("input");
    alsoInput.isAlsoSelectedInput = true;
    REQUIRE_FALSE (OutputDeviceSelector::isEligible (alsoInput));
}
