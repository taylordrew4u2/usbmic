#include "TestFramework.h"
#include "Core/BackgroundRecordWriter.h"
#include "Core/SessionRecordSchedule.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace mma;

namespace {

/// A drive the test controls: every write waits until released.
struct GatedDrive
{
    std::mutex mutex;
    std::condition_variable changed;
    bool open = false;
    int entered = 0;
    std::vector<std::string> landed;

    bool write (const std::string& path, const std::string& text)
    {
        std::unique_lock<std::mutex> lock (mutex);
        ++entered;
        changed.notify_all();
        changed.wait (lock, [this] { return open; });
        landed.push_back (path + "=" + text);
        return true;
    }

    void waitForEntered (int n)
    {
        std::unique_lock<std::mutex> lock (mutex);
        changed.wait_for (lock, std::chrono::seconds (5), [&] { return entered >= n; });
    }

    void release()
    {
        const std::lock_guard<std::mutex> lock (mutex);
        open = true;
        changed.notify_all();
    }
};

bool waitUntilIdle (const BackgroundRecordWriter& writer)
{
    for (int i = 0; i < 500; ++i)
    {
        if (! writer.isBusy())
            return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return false;
}

} // namespace

// A refresh that is still inside a slow drive must never land after the
// stop-time record: that would put "no stop time" back over a finished take,
// and the next launch would offer it as interrupted.
TEST_CASE (BackgroundRecordWriter_NothingLandsAfterRetireSaysSo)
{
    auto drive = std::make_shared<GatedDrive>();
    BackgroundRecordWriter writer ([drive] (const std::string& p, const std::string& t) { return drive->write (p, t); });

    writer.submit ("session.json", "first");
    drive->waitForEntered (1);

    // Queued behind the slow one, then the take stops.
    writer.submit ("session.json", "second");

    // The drive is stuck: retire must not wait past its bound, and says so.
    const auto started = std::chrono::steady_clock::now();
    REQUIRE_FALSE (writer.retire (std::chrono::milliseconds (100)));
    REQUIRE (std::chrono::steady_clock::now() - started < std::chrono::seconds (2));

    drive->release();
    REQUIRE (waitUntilIdle (writer));

    // The write already inside the drive finished; the queued one never ran.
    REQUIRE (drive->landed.size() == 1u);
    REQUIRE (drive->landed[0] == "session.json=first");

    // And nothing submitted after Stop is written.
    writer.submit ("session.json", "late");
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    REQUIRE (drive->landed.size() == 1u);

    // With the drive answering, retire is immediate and true.
    REQUIRE (writer.retire (std::chrono::milliseconds (100)));
}

// The caller never waits on the drive, and a drive slower than the refresh
// rate gets the newest record, not a backlog.
TEST_CASE (BackgroundRecordWriter_SubmitNeverWaitsAndTheNewestWins)
{
    auto drive = std::make_shared<GatedDrive>();
    BackgroundRecordWriter writer ([drive] (const std::string& p, const std::string& t) { return drive->write (p, t); });

    writer.submit ("a", "1");
    drive->waitForEntered (1);

    const auto started = std::chrono::steady_clock::now();
    for (int i = 2; i <= 50; ++i)
        writer.submit ("a", std::to_string (i));
    REQUIRE (std::chrono::steady_clock::now() - started < std::chrono::milliseconds (500));

    drive->release();
    REQUIRE (waitUntilIdle (writer));

    REQUIRE (drive->landed.size() == 2u);
    REQUIRE (drive->landed[0] == "a=1");
    REQUIRE (drive->landed[1] == "a=50");
    REQUIRE (writer.retire (std::chrono::milliseconds (100)));
}

// The owner going away -- the take's writers replaced by the next take's, or
// the app quitting -- never waits on the drive, and nothing queued behind a
// stuck write is written afterwards on its behalf.
TEST_CASE (BackgroundRecordWriter_GoingAwayNeverWaitsAndDropsWhatWasQueued)
{
    auto drive = std::make_shared<GatedDrive>();
    auto writer = std::make_unique<BackgroundRecordWriter> (
        [drive] (const std::string& p, const std::string& t) { return drive->write (p, t); });

    writer->submit ("session.json", "first");
    drive->waitForEntered (1);
    writer->submit ("session.json", "queued");

    const auto started = std::chrono::steady_clock::now();
    writer.reset();
    REQUIRE (std::chrono::steady_clock::now() - started < std::chrono::milliseconds (500));

    drive->release();

    // The worker outlives its owner, finishes the write it was in, and stops.
    for (int i = 0; i < 100; ++i)
    {
        {
            const std::lock_guard<std::mutex> lock (drive->mutex);
            if (! drive->landed.empty())
                break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    std::this_thread::sleep_for (std::chrono::milliseconds (100));

    const std::lock_guard<std::mutex> lock (drive->mutex);
    REQUIRE (drive->landed.size() == 1u);
    REQUIRE (drive->landed[0] == "session.json=first");
    REQUIRE (drive->entered == 1);
}

// A drive that keeps up gets every refresh, in order, and the last one is the
// record left on it.
TEST_CASE (BackgroundRecordWriter_ADriveThatKeepsUpGetsEveryRefresh)
{
    std::atomic<int> writes { 0 };
    std::string landed;
    std::mutex landedMutex;

    BackgroundRecordWriter writer ([&] (const std::string&, const std::string& text)
    {
        const std::lock_guard<std::mutex> lock (landedMutex);
        landed = text;
        ++writes;
        return true;
    });

    writer.submit ("session.json", "{\"dropouts\":[]}");
    REQUIRE (waitUntilIdle (writer));
    writer.submit ("session.json", "{\"dropouts\":[{\"device\":\"mic2\"}]}");
    REQUIRE (waitUntilIdle (writer));
    REQUIRE (writer.retire (std::chrono::milliseconds (100)));

    const std::lock_guard<std::mutex> lock (landedMutex);
    REQUIRE (writes.load() == 2);
    REQUIRE (landed == "{\"dropouts\":[{\"device\":\"mic2\"}]}");
}

// When a running take's record is rewritten: soon after anything it counts
// changes, and every thirty seconds regardless -- never before the take's own
// first write, and never after it has stopped.
TEST_CASE (SessionRecordSchedule_RefreshesOnChangeAndEveryPeriod)
{
    SessionRecordSchedule schedule;

    // Nothing is a refresh before the start-time record exists.
    REQUIRE_FALSE (schedule.isDue (100.0, "0|0|1"));

    schedule.written (100.0, "0|0|1");
    REQUIRE_FALSE (schedule.isDue (100.5, "0|0|1"));
    REQUIRE_FALSE (schedule.isDue (100.0 + SessionRecordSchedule::kPeriodSeconds - 0.5, "0|0|1"));

    // A microphone dropped out: due at once, not at the next period.
    REQUIRE (schedule.isDue (101.0, "1|0|1"));
    schedule.written (101.0, "1|0|1");
    REQUIRE_FALSE (schedule.isDue (101.5, "1|0|1"));

    // Quiet: due when a period has passed since the last write, not the first.
    REQUIRE_FALSE (schedule.isDue (101.0 + SessionRecordSchedule::kPeriodSeconds - 0.1, "1|0|1"));
    REQUIRE (schedule.isDue (101.0 + SessionRecordSchedule::kPeriodSeconds, "1|0|1"));

    // Stopped: the stop-time record is the last word.
    schedule.stop();
    REQUIRE_FALSE (schedule.isDue (500.0, "2|1|0"));

    // And the next take starts over from its own first write.
    schedule.written (600.0, "0|0|1");
    REQUIRE_FALSE (schedule.isDue (601.0, "0|0|1"));
    REQUIRE (schedule.isDue (601.0, "0|1|1"));
}
