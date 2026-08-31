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

TEST_CASE (DeviceManager_DefaultMasterUsesEnumerationOrderBeforeDriftMeasured)
{
    DeviceManager dm;
    dm.addDevice (makeDevice ("mic-b", 1));
    dm.addDevice (makeDevice ("mic-a", 0));

    const auto* master = dm.selectDefaultMaster();
    REQUIRE (master != nullptr);
    REQUIRE (master->identity.locationId == "mic-a");
}

TEST_CASE (DeviceManager_DefaultMasterPrefersLowestMeasuredDrift)
{
    DeviceManager dm;
    dm.addDevice (makeDevice ("mic-a", 0, 50.0, true));
    dm.addDevice (makeDevice ("mic-b", 1, 5.0, true));

    const auto* master = dm.selectDefaultMaster();
    REQUIRE (master != nullptr);
    REQUIRE (master->identity.locationId == "mic-b");
}

TEST_CASE (DeviceManager_FailoverPromotesLowestDriftAmongRemaining)
{
    // §3.3 failover, which needs no separate entry point: removeDevice() erases
    // the master, so the ordinary §3.1 rule can no longer return it and the
    // lowest-drift survivor wins by itself.
    //
    // There used to be a selectFailoverMaster() alongside this that took the
    // removed master's identity and skipped it explicitly. It only ever
    // excluded a device that was already gone, and it reimplemented §3.1
    // incompletely while doing so -- see the two cases below, both of which it
    // got wrong. It had no production callers and is removed.
    DeviceManager dm;
    dm.addDevice (makeDevice ("mic-a", 0, 2.0, true));
    dm.addDevice (makeDevice ("mic-b", 1, 30.0, true));
    dm.addDevice (makeDevice ("mic-c", 2, 10.0, true));

    PortIdentity removedMaster;
    removedMaster.locationId = "mic-a";
    REQUIRE (dm.removeDevice (removedMaster));

    const auto* newMaster = dm.selectDefaultMaster();
    REQUIRE (newMaster != nullptr);
    REQUIRE (newMaster->identity.locationId == "mic-c");
}

TEST_CASE (DeviceManager_FailoverStillHonoursTheUsersChosenMaster)
{
    // §3.1: the Advanced-panel override holds until *that* device leaves the
    // rig. Losing some other master must not quietly discard it.
    DeviceManager dm;
    dm.addDevice (makeDevice ("mic-a", 0, 2.0, true));
    dm.addDevice (makeDevice ("mic-b", 1, 30.0, true));
    dm.addDevice (makeDevice ("mic-c", 2, 10.0, true));

    dm.setPreferredMaster ("mic-b");

    PortIdentity gone;
    gone.locationId = "mic-a";
    REQUIRE (dm.removeDevice (gone));

    // Not mic-c, which is what picking purely on drift would give.
    REQUIRE (dm.selectDefaultMaster()->identity.locationId == "mic-b");
}

TEST_CASE (DeviceManager_FailoverStillPrefersAUsbMicOverTheBuiltIn)
{
    // §3.1: the machine's own microphone is a legitimate input and a poor
    // timebase. Failing over is no reason to hand it the clock.
    DeviceManager dm;

    MicDeviceState builtIn;
    builtIn.identity.locationId = "built-in";
    builtIn.isBuiltIn = true;
    builtIn.measuredDriftPpm = 1.0;
    builtIn.hasDriftMeasurement = true;
    dm.addDevice (builtIn);

    dm.addDevice (makeDevice ("mic-a", 1, 2.0, true));
    dm.addDevice (makeDevice ("mic-b", 2, 40.0, true));

    PortIdentity gone;
    gone.locationId = "mic-a";
    REQUIRE (dm.removeDevice (gone));

    // The built-in has the lowest measured drift of what is left, and still
    // loses to the remaining USB mic.
    REQUIRE (dm.selectDefaultMaster()->identity.locationId == "mic-b");
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

TEST_CASE (DeviceManager_PreferredMasterOverridesTheLowestDriftRule)
{
    DeviceManager m;

    MicDeviceState a;
    a.identity.locationId = "port-a";
    a.displayName = "A";
    a.enumerationOrder = 0;
    a.measuredDriftPpm = 1.0;
    a.hasDriftMeasurement = true;
    m.addDevice (a);

    MicDeviceState b;
    b.identity.locationId = "port-b";
    b.displayName = "B";
    b.enumerationOrder = 1;
    b.measuredDriftPpm = 50.0;
    b.hasDriftMeasurement = true;
    m.addDevice (b);

    // §3.1 automatic rule picks the lowest drift.
    REQUIRE (m.selectDefaultMaster()->displayName == "A");

    // §3.1 also lets the user say otherwise from the Advanced panel.
    m.setPreferredMaster ("port-b");
    REQUIRE (m.selectDefaultMaster()->displayName == "B");
}

TEST_CASE (DeviceManager_PreferredMasterThatLeavesFallsBackToTheRule)
{
    DeviceManager m;

    MicDeviceState a;
    a.identity.locationId = "port-a";
    a.displayName = "A";
    m.addDevice (a);

    MicDeviceState b;
    b.identity.locationId = "port-b";
    b.displayName = "B";
    b.enumerationOrder = 1;
    m.addDevice (b);

    m.setPreferredMaster ("port-b");
    REQUIRE (m.selectDefaultMaster()->displayName == "B");

    // §3.3: unplugging the chosen master must leave a master, not none.
    PortIdentity gone;
    gone.locationId = "port-b";
    m.removeDevice (gone);

    REQUIRE (m.selectDefaultMaster() != nullptr);
    REQUIRE (m.selectDefaultMaster()->displayName == "A");
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

TEST_CASE (DeviceManager_MasterSelectionUsesMeasuredDriftOnceAvailable)
{
    DeviceManager m;

    MicDeviceState a;
    a.identity.locationId = "port-a";
    a.displayName = "A";
    m.addDevice (a);

    MicDeviceState b;
    b.identity.locationId = "port-b";
    b.displayName = "B";
    b.enumerationOrder = 1;
    m.addDevice (b);

    // §3.1: before any measurement exists the tiebreak is enumeration order.
    REQUIRE (m.selectDefaultMaster()->displayName == "A");

    // Once B is measured as the steadier clock, it becomes the master.
    m.updateMeasuredDrift ("port-b", 1.0, 60.0);
    m.updateMeasuredDrift ("port-a", 40.0, 60.0);

    REQUIRE (m.selectDefaultMaster()->displayName == "B");
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

TEST_CASE (DeviceManager_BuiltInMicIsNotChosenAsClockMaster)
{
    // Regression, reported from real hardware: "clock master is always the
    // computer". CoreAudio enumerates the built-in microphone first, and with
    // no drift measurement yet the default master fell back to enumeration
    // order -- so the machine's own mic won on every Mac. §3.1 wants the
    // timebase to be something the user plugged in.
    DeviceManager m;
    m.syncToEnumeration ({ makeDevice ("uid-builtin", "Built-in Microphone", true),
                           makeDevice ("uid-yeti", "Yeti Stereo Microphone") });

    const auto* master = m.selectDefaultMaster();
    REQUIRE (master != nullptr);
    REQUIRE (master->identity.locationId == "uid-yeti");
}

TEST_CASE (DeviceManager_BuiltInMicIsStillUsedWhenItIsTheOnlyDevice)
{
    // A built-in master beats no master at all.
    DeviceManager m;
    m.syncToEnumeration ({ makeDevice ("uid-builtin", "Built-in Microphone", true) });

    const auto* master = m.selectDefaultMaster();
    REQUIRE (master != nullptr);
    REQUIRE (master->identity.locationId == "uid-builtin");
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

TEST_CASE (DeviceManager_DeselectedMicIsNotChosenAsClockMaster)
{
    DeviceManager m;
    m.syncToEnumeration ({ makeDevice ("uid-a", "Yeti A"), makeDevice ("uid-b", "Yeti B") });
    m.setUserEnabled ("uid-a", false);

    const auto* master = m.selectDefaultMaster();
    REQUIRE (master != nullptr);
    REQUIRE (master->identity.locationId == "uid-b");
}
