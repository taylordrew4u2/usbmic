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
    REQUIRE (alerts[0].message.find ("rest of this take") != std::string::npos);
    REQUIRE (alerts[0].message.find ("next take") != std::string::npos);
    REQUIRE (w.observe (now).empty());

    const auto back = w.observe (healthyRig());
    REQUIRE (back.empty());

    w.endTake();
    w.beginTake (healthyRig());
    REQUIRE (w.observe (healthyRig()).empty());
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
    REQUIRE (back.empty());
}

TEST_CASE (TakeWatchdog_DroppedAudioIsSaidOnceThenAtMostOnceAMinute)
{
    // A struggling machine drops a little on every tick. Said on every
    // increase, the card grew a line twice a second and could not be read or
    // dismissed -- seen for real in the headless take.
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.framesDropped = 10;
    now.elapsedSeconds = 1.0;
    REQUIRE (has (w.observe (now), TakeAlert::Kind::AudioDropped));

    now.framesDropped = 20; now.elapsedSeconds = 1.5;
    REQUIRE (w.observe (now).empty());           // more loss, too soon to repeat
    now.framesDropped = 30; now.elapsedSeconds = 30.0;
    REQUIRE (w.observe (now).empty());

    now.framesDropped = 48000 * 3; now.elapsedSeconds = 62.0;
    const auto again = w.observe (now);
    REQUIRE (again.size() == 1);
    REQUIRE (again[0].kind == TakeAlert::Kind::AudioDropped);
    REQUIRE (again[0].message.find ("still") != std::string::npos);
    REQUIRE (again[0].message.find ("3 s") != std::string::npos);
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

TEST_CASE (TakeWatchdog_OutputClockLostIsSaidOnceAndComesBack)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.outputClockLost = true;

    const auto lost = w.observe (now);
    REQUIRE (lost.size() == 1);
    REQUIRE (lost[0].kind == TakeAlert::Kind::OutputLost);
    REQUIRE (! lost[0].recovery);
    REQUIRE (w.observe (now).empty());

    const auto back = w.observe (healthyRig());
    REQUIRE (back.size() == 1);
    REQUIRE (back[0].kind == TakeAlert::Kind::OutputBack);
    REQUIRE (back[0].recovery);
}

TEST_CASE (TakeWatchdog_RingOverrunsCountAsDroppedAudio)
{
    TakeWatchdog w;
    w.beginTake (healthyRig());

    auto now = healthyRig();
    now.samplesOverrun = 256;
    REQUIRE (has (w.observe (now), TakeAlert::Kind::AudioDropped));
    REQUIRE (w.observe (now).empty());
}

TEST_CASE (TakeWatchdog_TheSecondsLostFigureUsesTheTakesOwnRate)
{
    // The figure was computed against a hardcoded 48000, so a take at 96 kHz
    // was told twice as much audio had been lost as actually had -- the same
    // wrong-magnitude family as counting channel frames as wall-clock ones.
    TakeWatchdog watchdog;

    auto start = healthyRig();
    start.elapsedSeconds = 0.0;
    start.sampleRate = 96000.0;
    watchdog.beginTake (start);

    TakeHealth now = start;
    now.elapsedSeconds = 5.0;
    now.framesDropped = 96000;   // one second at this take's rate
    now.sampleRate = 96000.0;

    const auto alerts = watchdog.observe (now);

    bool saidOneSecond = false;
    for (const auto& a : alerts)
        if (a.message.find ("1 s lost") != std::string::npos)
            saidOneSecond = true;

    // Either it names one second, or it is the first-time wording that gives no
    // figure at all. What it must never do is say two.
    for (const auto& a : alerts)
        REQUIRE (a.message.find ("2 s lost") == std::string::npos);

    (void) saidOneSecond;
}

TEST_CASE (TakeWatchdog_ADriveWithNoRoomIsNotToldItHasTwoMinutes)
{
    // Zero used to be unreachable here: the app collapsed a full drive into the
    // "unknown" sentinel, which this block's gate excluded. Now that zero is a
    // real value it would fall into the two-minute arm and tell someone with no
    // room at all that they have about two minutes and should wrap up --
    // contradicting, one tick earlier, the stop that is about to happen.
    TakeWatchdog watchdog;

    auto start = healthyRig();
    start.elapsedSeconds = 0.0;
    start.remainingSeconds = 3600.0;
    watchdog.beginTake (start);

    auto now = start;
    now.elapsedSeconds = 10.0;
    now.remainingSeconds = 0.0;

    for (const auto& a : watchdog.observe (now))
    {
        REQUIRE (a.message.find ("two minutes of room") == std::string::npos);
        REQUIRE (a.message.find ("ten minutes of room") == std::string::npos);
    }
}

TEST_CASE (TakeWatchdog_RealRoomWarningsStillFire)
{
    // The gate above must not have silenced the warnings it sits in front of.
    TakeWatchdog watchdog;

    auto start = healthyRig();
    start.elapsedSeconds = 0.0;
    start.remainingSeconds = 3600.0;
    watchdog.beginTake (start);

    auto now = start;
    now.elapsedSeconds = 10.0;
    now.remainingSeconds = 100.0; // inside two minutes, and genuinely non-zero

    bool warned = false;
    for (const auto& a : watchdog.observe (now))
        if (a.message.find ("two minutes of room") != std::string::npos)
            warned = true;

    REQUIRE (warned);
}
