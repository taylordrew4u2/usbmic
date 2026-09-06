#include "TestFramework.h"
#include "Core/TakeWatchdog.h"

using namespace mma;

namespace {

TakeHealth healthyRig()
{
    TakeHealth h;
    h.mics = { { "Alex", true }, { "Sam", true } };
    h.cameras = { { "FaceTime", true } };
    h.remainingSeconds = 3600.0;
    return h;
}

bool has (const std::vector<TakeAlert>& alerts, TakeAlert::Kind kind)
{
    for (const auto& a : alerts)
        if (a.kind == kind)
            return true;
    return false;
}

} // namespace

TEST_CASE (TakeWatchdog_NothingToSayWhenNothingChanges)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());
    REQUIRE (w.observe (healthyRig()).empty());
    REQUIRE (w.observe (healthyRig()).empty());
}

TEST_CASE (TakeWatchdog_SilentOutsideATake)
{
    TakeWatchdog w;
    auto bad = healthyRig();
    bad.mics[0].live = false;
    REQUIRE (w.observe (bad).empty());
}

TEST_CASE (TakeWatchdog_AMicGoingAwayIsSaidOnceAndNamed)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.mics[1].live = false;

    const auto alerts = w.observe (now);
    REQUIRE (alerts.size() == 1);
    REQUIRE (alerts[0].kind == TakeAlert::Kind::MicLost);
    REQUIRE (alerts[0].message.find ("Sam") != std::string::npos);
    REQUIRE (! alerts[0].recovery);

    // Still dead next tick: not said again.
    REQUIRE (w.observe (now).empty());
}

TEST_CASE (TakeWatchdog_AMicComingBackIsGoodNews)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto gone = healthyRig();
    gone.mics[0].live = false;
    w.observe (gone);

    const auto alerts = w.observe (healthyRig());
    REQUIRE (alerts.size() == 1);
    REQUIRE (alerts[0].kind == TakeAlert::Kind::MicBack);
    REQUIRE (alerts[0].recovery);
    REQUIRE (alerts[0].message.find ("Alex") != std::string::npos);
}

TEST_CASE (TakeWatchdog_AMicDeadAtTheStartIsNotAnAlert)
{
    // It was on screen before record was pressed; the pop-up is for changes.
    TakeWatchdog w;
    auto baseline = healthyRig();
    baseline.mics[0].live = false;
    w.beginTake (baseline);
    REQUIRE (w.observe (baseline).empty());
}

TEST_CASE (TakeWatchdog_ACameraGoingAwayIsNamed)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.cameras[0].present = false;

    const auto alerts = w.observe (now);
    REQUIRE (alerts.size() == 1);
    REQUIRE (alerts[0].kind == TakeAlert::Kind::CameraLost);
    REQUIRE (alerts[0].message.find ("FaceTime") != std::string::npos);
    REQUIRE (w.observe (now).empty());

    const auto back = w.observe (healthyRig());
    REQUIRE (back.size() == 1);
    REQUIRE (back[0].kind == TakeAlert::Kind::CameraBack);
}

TEST_CASE (TakeWatchdog_ACameraThatVanishesFromTheListIsGoneToo)
{
    // The OS stops listing an unplugged camera; it does not list it as absent.
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.cameras.clear();

    const auto alerts = w.observe (now);
    REQUIRE (alerts.size() == 1);
    REQUIRE (alerts[0].kind == TakeAlert::Kind::CameraLost);
    REQUIRE (alerts[0].message.find ("FaceTime") != std::string::npos);
    REQUIRE (w.observe (now).empty());

    const auto back = w.observe (healthyRig());
    REQUIRE (back.size() == 1);
    REQUIRE (back[0].kind == TakeAlert::Kind::CameraBack);
}

TEST_CASE (TakeWatchdog_DroppedAudioIsReportedOnEveryIncrease)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.framesDropped = 10;
    REQUIRE (has (w.observe (now), TakeAlert::Kind::AudioDropped));
    REQUIRE (w.observe (now).empty());          // no new drop, no new alert

    now.framesDropped = 20;
    REQUIRE (has (w.observe (now), TakeAlert::Kind::AudioDropped));
}

TEST_CASE (TakeWatchdog_DriveTroubleIsSaidWhenItStarts)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.writerBehind = true;
    REQUIRE (has (w.observe (now), TakeAlert::Kind::WriterBehind));
    REQUIRE (w.observe (now).empty());

    now.mixOnly = true;
    REQUIRE (has (w.observe (now), TakeAlert::Kind::MixOnly));
}

TEST_CASE (TakeWatchdog_RoomWarningsFireOncePerThresholdOnTheWayDown)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.remainingSeconds = 599.0;
    REQUIRE (has (w.observe (now), TakeAlert::Kind::TenMinutesLeft));
    now.remainingSeconds = 500.0;
    REQUIRE (w.observe (now).empty());

    now.remainingSeconds = 119.0;
    const auto two = w.observe (now);
    REQUIRE (two.size() == 1);
    REQUIRE (two[0].kind == TakeAlert::Kind::TwoMinutesLeft);
    now.remainingSeconds = 60.0;
    REQUIRE (w.observe (now).empty());
}

TEST_CASE (TakeWatchdog_UnknownRoomIsNotAWarning)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());
    auto now = healthyRig();
    now.remainingSeconds = -1.0;
    REQUIRE (w.observe (now).empty());
}

TEST_CASE (TakeWatchdog_ANewTakeStartsClean)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());
    auto low = healthyRig();
    low.remainingSeconds = 100.0;
    w.observe (low);
    w.endTake();

    // The next take on the same nearly-full drive warns again: the warning
    // belongs to the take, not to the drive.
    w.beginTake (healthyRig());
    REQUIRE (has (w.observe (low), TakeAlert::Kind::TwoMinutesLeft));
}
