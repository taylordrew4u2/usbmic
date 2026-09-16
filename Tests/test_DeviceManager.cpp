#include "TestFramework.h"
#include "Core/DeviceManager.h"

using namespace mma;

static MicDeviceState makeDevice (const std::string& loc, int order, double drift = 0.0, bool hasDrift = false)
{
    MicDeviceState d;
    d.identity.locationId = loc;
    d.displayName = loc;
    d.enumerationOrder = order;
    d.measuredDriftPpm = drift;
    d.hasDriftMeasurement = hasDrift;
    return d;
}

TEST_CASE (DeviceManager_UpToEightMicsAllIncluded)
{
    DeviceManager dm;
    for (int i = 0; i < 8; ++i)
        dm.addDevice (makeDevice ("mic" + std::to_string (i), i));

    for (const auto& d : dm.getDevices())
        REQUIRE (d.included);
}

TEST_CASE (DeviceManager_NinthMicExcludedWithReason)
{
    DeviceManager dm;
    for (int i = 0; i < 9; ++i)
        dm.addDevice (makeDevice ("mic" + std::to_string (i), i));

    int excludedCount = 0;
    for (const auto& d : dm.getDevices())
    {
        if (! d.included)
        {
            ++excludedCount;
            REQUIRE_FALSE (d.exclusionReason.empty());
        }
    }
    REQUIRE (excludedCount == 1);
}











TEST_CASE (DeviceManager_RemoveDeviceReturnsTrueWhenFound)
{
    DeviceManager dm;
    dm.addDevice (makeDevice ("mic-a", 0));
    PortIdentity id;
    id.locationId = "mic-a";
    REQUIRE (dm.removeDevice (id));
    REQUIRE (dm.getDevices().empty());
}

TEST_CASE (DeviceManager_RemoveDeviceReturnsFalseWhenNotFound)
{
    DeviceManager dm;
    PortIdentity id;
    id.locationId = "nonexistent";
    REQUIRE_FALSE (dm.removeDevice (id));
}

TEST_CASE (DeviceManager_UnpluggingFreesASlotForTheNinthMic)
{
    DeviceManager dm;
    for (int i = 0; i < 8; ++i)
        dm.addDevice (makeDevice ("mic" + std::to_string (i), i));
    dm.addDevice (makeDevice ("mic8", 8)); // 9th, excluded

    PortIdentity id0;
    id0.locationId = "mic0";
    dm.removeDevice (id0);

    bool ninthNowIncluded = false;
    for (const auto& d : dm.getDevices())
        if (d.identity.locationId == "mic8")
            ninthNowIncluded = d.included;

    REQUIRE (ninthNowIncluded);
}





TEST_CASE (DeviceManager_DriftIsNotClaimedBeforeTheMeasurementWindow)
{
    DeviceManager m;

    MicDeviceState a;
    a.identity.locationId = "port-a";
    a.displayName = "A";
    m.addDevice (a);

    // §3.1: 60 seconds of running measurement, not 59.
    m.updateMeasuredDrift ("port-a", 12.5, 59.0);
    REQUIRE_FALSE (m.getDevices()[0].hasDriftMeasurement);
    REQUIRE_NEAR (m.getDevices()[0].measuredDriftPpm, 12.5, 1e-9);

    m.updateMeasuredDrift ("port-a", 12.5, 60.0);
    REQUIRE (m.getDevices()[0].hasDriftMeasurement);
}



namespace {

MicDeviceState makeDevice (const std::string& uid, const std::string& name, bool builtIn = false)
{
    MicDeviceState d;
    d.identity.locationId = uid;
    d.displayName = name;
    d.isBuiltIn = builtIn;
    return d;
}

} // namespace

TEST_CASE (DeviceManager_ReEnumerationDoesNotDuplicateDevices)
{
    // Regression, reported from real hardware: one Yeti appeared five times in
    // the Advanced panel. macOS fires its device-list listener several times
    // while a USB microphone initialises, and the handler called addDevice()
    // for every enumerated device on every firing, appending unconditionally.
    DeviceManager m;

    const std::vector<MicDeviceState> enumeration {
        makeDevice ("uid-builtin", "Built-in Microphone", true),
        makeDevice ("uid-yeti-1", "Yeti Stereo Microphone"),
    };

    for (int firing = 0; firing < 5; ++firing)
        m.syncToEnumeration (enumeration);

    REQUIRE (m.getDevices().size() == 2);
}

TEST_CASE (DeviceManager_AddDeviceRejectsADuplicateIdentity)
{
    DeviceManager m;
    REQUIRE (m.addDevice (makeDevice ("uid-a", "Yeti Stereo Microphone")));
    REQUIRE_FALSE (m.addDevice (makeDevice ("uid-a", "Yeti Stereo Microphone")));
    REQUIRE (m.getDevices().size() == 1);
}

TEST_CASE (DeviceManager_TwoIdenticalModelsStayDistinct)
{
    // Dedup keys on identity, never on name. Two Yetis report the same product
    // name and must remain two devices.
    DeviceManager m;
    m.syncToEnumeration ({ makeDevice ("uid-yeti-1", "Yeti Stereo Microphone"),
                           makeDevice ("uid-yeti-2", "Yeti Stereo Microphone") });

    REQUIRE (m.getDevices().size() == 2);
}

TEST_CASE (DeviceManager_SyncAddsNewAndDropsRemovedDevices)
{
    DeviceManager m;
    m.syncToEnumeration ({ makeDevice ("uid-a", "Yeti A"), makeDevice ("uid-b", "Yeti B") });
    REQUIRE (m.getDevices().size() == 2);

    // B unplugged, C plugged in.
    m.syncToEnumeration ({ makeDevice ("uid-a", "Yeti A"), makeDevice ("uid-c", "Yeti C") });
    REQUIRE (m.getDevices().size() == 2);

    bool hasA = false, hasB = false, hasC = false;
    for (const auto& d : m.getDevices())
    {
        if (d.identity.locationId == "uid-a") hasA = true;
        if (d.identity.locationId == "uid-b") hasB = true;
        if (d.identity.locationId == "uid-c") hasC = true;
    }
    REQUIRE (hasA);
    REQUIRE_FALSE (hasB);
    REQUIRE (hasC);
}

TEST_CASE (DeviceManager_SurvivingDeviceKeepsItsEnumerationOrder)
{
    // A device that never left is not a new device: re-enumeration must not
    // reshuffle it, or the 8-mic cap would reorder itself on every hotplug.
    DeviceManager m;
    m.syncToEnumeration ({ makeDevice ("uid-a", "Yeti A"), makeDevice ("uid-b", "Yeti B") });

    int orderOfA = -1;
    for (const auto& d : m.getDevices())
        if (d.identity.locationId == "uid-a")
            orderOfA = d.enumerationOrder;

    m.syncToEnumeration ({ makeDevice ("uid-a", "Yeti A"),
                           makeDevice ("uid-b", "Yeti B"),
                           makeDevice ("uid-c", "Yeti C") });

    for (const auto& d : m.getDevices())
        if (d.identity.locationId == "uid-a")
            REQUIRE (d.enumerationOrder == orderOfA);
}





TEST_CASE (DeviceManager_DeselectedMicIsExcludedWithAReason)
{
    DeviceManager m;
    m.syncToEnumeration ({ makeDevice ("uid-a", "Yeti A"), makeDevice ("uid-b", "Yeti B") });

    REQUIRE (m.setUserEnabled ("uid-a", false));

    for (const auto& d : m.getDevices())
    {
        if (d.identity.locationId != "uid-a")
            continue;

        // `included` is the flag every capture, metering and drift path already
        // consults, so clearing it is what actually stops the mic being recorded.
        REQUIRE_FALSE (d.included);
        REQUIRE_FALSE (d.exclusionReason.empty());
    }
}

TEST_CASE (DeviceManager_SettingTheSameSelectionTwiceReportsNoChange)
{
    // The caller tears down and rebuilds the audio streams on a change, which
    // is far too expensive to do for a click that changed nothing.
    DeviceManager m;
    m.syncToEnumeration ({ makeDevice ("uid-a", "Yeti A") });

    REQUIRE (m.setUserEnabled ("uid-a", false));
    REQUIRE_FALSE (m.setUserEnabled ("uid-a", false));
    REQUIRE (m.setUserEnabled ("uid-a", true));
}

TEST_CASE (DeviceManager_DeselectedMicDoesNotConsumeOneOfTheEightSlots)
{
    // Deselecting a mic you are not using should make room for one you are.
    DeviceManager m;
    std::vector<MicDeviceState> nine;
    for (int i = 0; i < 9; ++i)
        nine.push_back (makeDevice ("uid-" + std::to_string (i), "Mic " + std::to_string (i)));

    m.syncToEnumeration (nine);

    int included = 0;
    for (const auto& d : m.getDevices())
        if (d.included)
            ++included;
    REQUIRE (included == DeviceManager::kMaxMicrophones);

    // Turning the first one off should let the ninth in.
    m.setUserEnabled ("uid-0", false);

    bool ninthIncluded = false;
    included = 0;
    for (const auto& d : m.getDevices())
    {
        if (d.included)
            ++included;
        if (d.identity.locationId == "uid-8" && d.included)
            ninthIncluded = true;
    }

    REQUIRE (included == DeviceManager::kMaxMicrophones);
    REQUIRE (ninthIncluded);
}

TEST_CASE (DeviceManager_SelectionSurvivesReEnumeration)
{
    // A hotplug elsewhere in the rig must not silently re-enable a microphone
    // the user turned off.
    DeviceManager m;
    m.syncToEnumeration ({ makeDevice ("uid-a", "Yeti A"), makeDevice ("uid-b", "Yeti B") });
    m.setUserEnabled ("uid-a", false);

    m.syncToEnumeration ({ makeDevice ("uid-a", "Yeti A"),
                           makeDevice ("uid-b", "Yeti B"),
                           makeDevice ("uid-c", "Yeti C") });

    for (const auto& d : m.getDevices())
        if (d.identity.locationId == "uid-a")
            REQUIRE_FALSE (d.userEnabled);
}



TEST_CASE (DeviceManager_ReEnumerationRefreshesHowManyInputsADeviceHas)
{
    // How many inputs a device presents belongs to the OS, and a device the
    // manager already knows must have it refreshed like any other OS-owned
    // fact.
    //
    // It was not, and that silently defeated the whole multi-input feature: an
    // interface added through addDevice(), or present before the first
    // enumeration, kept the default of one input forever. The app went on
    // building one take channel for a four-microphone interface, which is the
    // exact bug the channel count exists to fix.
    DeviceManager m;

    MicDeviceState early;
    early.identity.locationId = "loc-interface";
    early.displayName = "Interface";
    early.inputChannelCount = 1;   // what any path that does not know better sets
    m.addDevice (early);

    MicDeviceState reported;
    reported.identity.locationId = "loc-interface";
    reported.displayName = "Interface";
    reported.inputChannelCount = 4; // what the OS actually says

    m.syncToEnumeration ({ reported });

    REQUIRE (m.getDevices().size() == 1);
    REQUIRE (m.getDevices()[0].inputChannelCount == 4);
}

TEST_CASE (DeviceManager_AnInterfaceContributesOneChannelPerInput)
{
    // The rule that decides how many strips and files a device produces.
    //
    // One input is one microphone. Two stays one -- §2.1's USB mic presenting
    // the same voice on both sides, which the capture path's analyzer resolves.
    // Above two, the device is an interface and every input is somebody's
    // microphone, each of whom expects their own track.
    REQUIRE (takeChannelsForDevice (1) == 1);
    REQUIRE (takeChannelsForDevice (4) == 4);
    REQUIRE (takeChannelsForDevice (8) == 8);

    // Two inputs are two microphones until proven otherwise. A two-input
    // interface with two people plugged into it is the commonest small
    // multi-mic rig there is, and this used to collapse it to one, throwing a
    // person away on the assumption that any two-channel device is a stereo
    // USB mic.
    REQUIRE (takeChannelsForDevice (2) == 2);

    // Collapsed only once §2.1 has looked at the audio and said the two sides
    // carry the same source -- §2.4 remembers that per port.
    REQUIRE (takeChannelsForDevice (2, true) == 1);

    // A decision nobody made is not evidence.
    REQUIRE (takeChannelsForDevice (2, false) == 2);

    // The verdict only ever collapses a stereo pair. It cannot cut an
    // interface down, whatever is remembered about it.
    REQUIRE (takeChannelsForDevice (4, true) == 4);

    // A device reporting nothing usable is still one microphone, never zero:
    // zero channels is a take with no files in it.
    REQUIRE (takeChannelsForDevice (0) == 1);
    REQUIRE (takeChannelsForDevice (-1) == 1);
}
