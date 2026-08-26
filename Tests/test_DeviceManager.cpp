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
    DeviceManager dm;
    dm.addDevice (makeDevice ("mic-a", 0, 2.0, true));
    dm.addDevice (makeDevice ("mic-b", 1, 30.0, true));
    dm.addDevice (makeDevice ("mic-c", 2, 10.0, true));

    PortIdentity removedMaster;
    removedMaster.locationId = "mic-a";
    const auto* newMaster = dm.selectFailoverMaster (removedMaster);
    REQUIRE (newMaster != nullptr);
    REQUIRE (newMaster->identity.locationId == "mic-c");
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
