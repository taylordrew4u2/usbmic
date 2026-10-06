#include "TestFramework.h"
#include "Core/BatteryRecordingNotice.h"

using namespace mma;

namespace {

struct FakePower
{
    PowerSource source = PowerSource::AC;
    int reads = 0;

    std::function<PowerSource()> reader()
    {
        return [this]
        {
            ++reads;
            return source;
        };
    }
};

} // namespace

TEST_CASE (BatteryRecordingNotice_WarnsOnceWhenATakeStartsOnBattery)
{
    FakePower power;
    power.source = PowerSource::Battery;
    BatteryRecordingNotice notice;

    // The first tick of the take reads at once, without waiting a recheck.
    REQUIRE (notice.tick (true, 0.5, power.reader()));

    // Once per take, however long it stays on battery.
    for (int i = 0; i < 100; ++i)
        REQUIRE_FALSE (notice.tick (true, 0.5, power.reader()));

    REQUIRE (power.reads == 1);
}

TEST_CASE (BatteryRecordingNotice_SilentOnACAndWhenThePowerIsUnknown)
{
    for (const auto source : { PowerSource::AC, PowerSource::Unknown })
    {
        FakePower power;
        power.source = source;
        BatteryRecordingNotice notice;

        for (int i = 0; i < 40; ++i)
            REQUIRE_FALSE (notice.tick (true, 0.5, power.reader()));
    }
}

TEST_CASE (BatteryRecordingNotice_ChargerPulledMidTakeIsNoticedWithinARecheck)
{
    FakePower power;
    BatteryRecordingNotice notice;

    REQUIRE_FALSE (notice.tick (true, 0.5, power.reader()));
    power.source = PowerSource::Battery;

    double waited = 0.0;
    bool warned = false;
    while (! warned && waited < 30.0)
    {
        warned = notice.tick (true, 0.5, power.reader());
        waited += 0.5;
    }

    REQUIRE (warned);
    REQUIRE (waited <= BatteryRecordingNotice::kRecheckSeconds + 0.5);

    // Charger back in and out again: still the same take, still said once.
    power.source = PowerSource::AC;
    for (int i = 0; i < 20; ++i)
        REQUIRE_FALSE (notice.tick (true, 0.5, power.reader()));
    power.source = PowerSource::Battery;
    for (int i = 0; i < 20; ++i)
        REQUIRE_FALSE (notice.tick (true, 0.5, power.reader()));
}

TEST_CASE (BatteryRecordingNotice_NeverAsksBetweenTakesAndReArmsForTheNext)
{
    FakePower power;
    power.source = PowerSource::Battery;
    BatteryRecordingNotice notice;

    // Idle: no warning, and the OS is not asked at all.
    for (int i = 0; i < 20; ++i)
        REQUIRE_FALSE (notice.tick (false, 0.5, power.reader()));
    REQUIRE (power.reads == 0);

    REQUIRE (notice.tick (true, 0.5, power.reader()));
    REQUIRE_FALSE (notice.tick (false, 0.5, power.reader()));

    // The next take on battery is told again.
    REQUIRE (notice.tick (true, 0.5, power.reader()));
}

TEST_CASE (BatteryRecordingNotice_ReadsArePacedNotPerTick)
{
    FakePower power;
    BatteryRecordingNotice notice;

    // Sixty seconds of half-second ticks on AC.
    for (int i = 0; i < 120; ++i)
        notice.tick (true, 0.5, power.reader());

    // One at the start, then one per recheck interval.
    REQUIRE (power.reads <= 1 + static_cast<int> (60.0 / BatteryRecordingNotice::kRecheckSeconds));
    REQUIRE (power.reads >= static_cast<int> (60.0 / BatteryRecordingNotice::kRecheckSeconds));
}
