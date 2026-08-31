#include "TestFramework.h"
#include "../Source/Core/ClockMasterResolver.h"
#include "../Source/Core/DeviceManager.h"

using namespace mma;

namespace {

MicDeviceState mic (const std::string& location, int order, bool builtIn = false)
{
    MicDeviceState d;
    d.identity.locationId = location;
    d.displayName = location;
    d.isBuiltIn = builtIn;
    d.enumerationOrder = order;
    return d;
}

} // namespace

TEST_CASE (ClockMaster_ResolvesToTheTakesChannelIndexNotTheDeviceIndex)
{
    // Three microphones recording; B is unplugged mid-take. §6.5 keeps its
    // channel, so the take is still A(0) B(1) C(2) -- but the device list the
    // ranking comes from is now just A, C.
    const std::vector<std::string> take { "A", "B", "C" };
    const std::vector<bool> live { true, false, true };

    // C is the best surviving candidate. Counting included devices to find its
    // index yields 1, which is B -- the channel that is writing silence.
    const auto r = resolveMasterChannel (take, live, { "C", "A" });

    REQUIRE (r.channelIndex == 2);
    REQUIRE (r.deviceId == "C");
}

TEST_CASE (ClockMaster_SkipsAChannelThatIsWritingSilence)
{
    // The top-ranked device is in the take but no longer delivering: locking to
    // it would resample every other microphone onto silence.
    const auto r = resolveMasterChannel ({ "A", "B" }, { false, true }, { "A", "B" });

    REQUIRE (r.channelIndex == 1);
    REQUIRE (r.deviceId == "B");
}

TEST_CASE (ClockMaster_SkipsADeviceThatIsNotInThisTake)
{
    // A microphone plugged in mid-take is present and healthy, and §6.5 says it
    // joins nothing until the next take. It must not be picked as the timebase
    // for a take that is not recording it.
    const auto r = resolveMasterChannel ({ "A", "B" }, { true, true }, { "NEW", "B" });

    REQUIRE (r.channelIndex == 1);
    REQUIRE (r.deviceId == "B");
}

TEST_CASE (ClockMaster_ReportsNoMasterWhenEveryChannelIsDead)
{
    const auto r = resolveMasterChannel ({ "A", "B" }, { false, false }, { "A", "B" });

    REQUIRE (r.channelIndex == -1);
    REQUIRE (r.deviceId.empty());
}

TEST_CASE (ClockMaster_MissingLivenessEntriesCountAsLive)
{
    // Defensive: a short liveness vector must not silently demote channels.
    const auto r = resolveMasterChannel ({ "A", "B" }, {}, { "B" });

    REQUIRE (r.channelIndex == 1);
}

TEST_CASE (ClockMaster_RankingPutsThePreferredDeviceFirstThenDriftThenBuiltIn)
{
    DeviceManager m;
    m.addDevice (mic ("builtin", 0, true));
    m.addDevice (mic ("usb-a", 1));
    m.addDevice (mic ("usb-b", 2));

    auto ranked = m.rankMasterCandidates();
    REQUIRE (ranked.size() == 3u);
    // No measurements yet: enumeration order among the USB mics, built-in last.
    REQUIRE (ranked[0]->identity.locationId == "usb-a");
    REQUIRE (ranked[1]->identity.locationId == "usb-b");
    REQUIRE (ranked[2]->identity.locationId == "builtin");

    // A measured drift on usb-b promotes it over the unmeasured usb-a.
    m.updateMeasuredDrift (mic ("usb-b", 2).identity.key(), 4.0,
                           DeviceManager::kDriftMeasurementSeconds + 1.0);
    ranked = m.rankMasterCandidates();
    REQUIRE (ranked[0]->identity.locationId == "usb-b");

    // The user's override outranks the rule while that device is present.
    m.setPreferredMaster (mic ("usb-a", 1).identity.key());
    ranked = m.rankMasterCandidates();
    REQUIRE (ranked[0]->identity.locationId == "usb-a");
    REQUIRE (ranked[1]->identity.locationId == "usb-b");
    // and appears exactly once.
    REQUIRE (ranked.size() == 3u);
}

TEST_CASE (ClockMaster_RankingAgreesWithTheSingleWinnerRule)
{
    DeviceManager m;
    m.addDevice (mic ("builtin", 0, true));
    m.addDevice (mic ("usb-a", 1));
    m.addDevice (mic ("usb-b", 2));

    // selectDefaultMaster is the front of the ranking, so a take that fails
    // over uses the same rule that chose the master in the first place.
    REQUIRE (m.selectDefaultMaster() == m.rankMasterCandidates().front());
}

TEST_CASE (ClockMaster_FailoverAfterTheMasterIsUnpluggedPicksTheNextRankedChannel)
{
    // The end-to-end shape of §3.3: the master leaves the device list entirely
    // (syncToEnumeration drops it) while its channel stays in the take.
    DeviceManager m;
    m.addDevice (mic ("usb-a", 0));
    m.addDevice (mic ("usb-b", 1));
    m.addDevice (mic ("usb-c", 2));

    const std::vector<std::string> take { m.getDevices()[0].identity.key(),
                                          m.getDevices()[1].identity.key(),
                                          m.getDevices()[2].identity.key() };

    // usb-a is unplugged: gone from the device list, still channel 0 of the take.
    m.syncToEnumeration ({ mic ("usb-b", 1), mic ("usb-c", 2) });

    std::vector<std::string> ranked;
    for (const auto* d : m.rankMasterCandidates())
        ranked.push_back (d->identity.key());

    const auto r = resolveMasterChannel (take, { false, true, true }, ranked);

    REQUIRE (r.channelIndex == 1);
    REQUIRE (r.deviceId == take[1]);
}
