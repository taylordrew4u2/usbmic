#include "TestFramework.h"
#include "Core/BackgroundRecordWriter.h"
#include "Core/SessionRecordSchedule.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace mma;

namespace {

/// A drive the test controls. Each text's temporary copy, and then its
/// rename, waits until the test lets that text (or everything) through; what
/// was put in place is kept in order, and so is what the writer refused.
/// Shared with the writer's workers, which can outlive a test's writer.
struct ControlledDrive : std::enable_shared_from_this<ControlledDrive>
{
    std::mutex mutex;
    std::condition_variable changed;
    bool open = false;
    std::set<std::string> copyOpen, renameOpen;
    std::vector<std::string> copying, renaming, landed, refused;

    bool write (const std::string& path, const std::string& text, const BackgroundRecordWriter::Commit& commit)
    {
        {
            std::unique_lock<std::mutex> lock (mutex);
            copying.push_back (text);
            changed.notify_all();
            changed.wait (lock, [&] { return open || copyOpen.count (text) > 0; });
        }

        const bool putInPlace = commit ([&]
        {
            std::unique_lock<std::mutex> lock (mutex);
            renaming.push_back (text);
            changed.notify_all();
            changed.wait (lock, [&] { return open || renameOpen.count (text) > 0; });
            landed.push_back (path + "=" + text);
            changed.notify_all();
            return true;
        });

        if (! putInPlace)
        {
            const std::lock_guard<std::mutex> lock (mutex);
            refused.push_back (text);
            changed.notify_all();
        }

        return putInPlace;
    }

    BackgroundRecordWriter::WriteFunction function()
    {
        return [self = shared_from_this()] (const std::string& p, const std::string& t,
                                            const BackgroundRecordWriter::Commit& c)
        {
            return self->write (p, t, c);
        };
    }

    void release()
    {
        const std::lock_guard<std::mutex> lock (mutex);
        open = true;
        changed.notify_all();
    }

    void letCopy (const std::string& text)
    {
        const std::lock_guard<std::mutex> lock (mutex);
        copyOpen.insert (text);
        changed.notify_all();
    }

    void letRename (const std::string& text)
    {
        const std::lock_guard<std::mutex> lock (mutex);
        renameOpen.insert (text);
        changed.notify_all();
    }

    static bool contains (const std::vector<std::string>& list, const std::string& text)
    {
        for (const auto& entry : list)
            if (entry == text)
                return true;
        return false;
    }

    bool waitUntil (const std::function<bool()>& condition)
    {
        std::unique_lock<std::mutex> lock (mutex);
        return changed.wait_for (lock, std::chrono::seconds (5), condition);
    }

    bool waitForCopying (const std::string& text)   { return waitUntil ([&] { return contains (copying, text); }); }
    bool waitForRenaming (const std::string& text)  { return waitUntil ([&] { return contains (renaming, text); }); }
    bool waitForRefused (const std::string& text)   { return waitUntil ([&] { return contains (refused, text); }); }
    bool waitForLanded (std::size_t n)              { return waitUntil ([&] { return landed.size() >= n; }); }

    std::vector<std::string> landedSoFar()
    {
        const std::lock_guard<std::mutex> lock (mutex);
        return landed;
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
// and the next launch would offer it as interrupted. One still writing its
// temporary copy does not hold Stop up: it is refused at its rename.
TEST_CASE (BackgroundRecordWriter_NothingLandsAfterRetireSaysSo)
{
    auto drive = std::make_shared<ControlledDrive>();
    BackgroundRecordWriter writer (drive->function());

    writer.submit ("session.json", "first");
    REQUIRE (drive->waitForCopying ("first"));

    // Queued behind the slow one, then the take stops.
    writer.submit ("session.json", "second");

    const auto started = std::chrono::steady_clock::now();
    REQUIRE (writer.retire (std::chrono::milliseconds (100)));
    REQUIRE (std::chrono::steady_clock::now() - started < std::chrono::seconds (2));

    drive->release();
    REQUIRE (drive->waitForRefused ("first"));
    REQUIRE (waitUntilIdle (writer));

    // The one in the drive was refused at its rename; the queued one never ran.
    REQUIRE (drive->landedSoFar().empty());

    // And nothing submitted after Stop is written.
    writer.submit ("session.json", "late");
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    REQUIRE (drive->landedSoFar().empty());
}

// A refresh already inside its rename when the take stops cannot be called
// back: retire waits no longer than its bound and says so.
TEST_CASE (BackgroundRecordWriter_RetireSaysWhenARenameIsStuck)
{
    auto drive = std::make_shared<ControlledDrive>();
    BackgroundRecordWriter writer (drive->function());

    writer.submit ("session.json", "first");
    drive->letCopy ("first");
    REQUIRE (drive->waitForRenaming ("first"));
    writer.submit ("session.json", "queued");

    const auto started = std::chrono::steady_clock::now();
    REQUIRE_FALSE (writer.retire (std::chrono::milliseconds (100)));
    REQUIRE (std::chrono::steady_clock::now() - started < std::chrono::seconds (2));

    drive->release();
    REQUIRE (waitUntilIdle (writer));

    const auto landed = drive->landedSoFar();
    REQUIRE (landed.size() == 1u);
    REQUIRE (landed[0] == "session.json=first");

    // With the drive answering, retire is immediate and true.
    REQUIRE (writer.retire (std::chrono::milliseconds (100)));
}

// The backup copy's stop-time record goes through the same writer. A refresh
// still writing its temporary copy when the take stops is refused at its
// rename however late the drive lets it get there -- after the stop-time
// record is in place included -- and the stop-time record is not held up
// waiting for it.
TEST_CASE (BackgroundRecordWriter_ARefreshFinishingAfterTheLastRecordNeverLandsOnIt)
{
    auto drive = std::make_shared<ControlledDrive>();
    BackgroundRecordWriter writer (drive->function());

    writer.submit ("session.json", "refresh");
    REQUIRE (drive->waitForCopying ("refresh"));
    writer.submit ("session.json", "queued");

    drive->letCopy ("stopped");
    drive->letRename ("stopped");

    const auto started = std::chrono::steady_clock::now();
    const auto written = writer.writeLast ("session.json", "stopped", std::chrono::milliseconds (2000));
    REQUIRE (std::chrono::steady_clock::now() - started < std::chrono::seconds (3));
    REQUIRE (written.has_value());
    REQUIRE (*written);

    // The slow drive finally lets the refresh through: refused at its rename.
    drive->release();
    REQUIRE (drive->waitForRefused ("refresh"));
    REQUIRE (waitUntilIdle (writer));

    const auto landed = drive->landedSoFar();
    REQUIRE (landed.size() == 1u);
    REQUIRE (landed[0] == "session.json=stopped");

    // Nothing after it, either.
    writer.submit ("session.json", "late");
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    REQUIRE (drive->landedSoFar().size() == 1u);
}

// A refresh already inside its rename on a drive that has stopped answering:
// the caller waits no longer than its bound and hears that the record is not
// in yet. The record is still written when the drive answers -- after the
// refresh, never before it, so it is what is left on the drive.
TEST_CASE (BackgroundRecordWriter_TheLastRecordWaitsBehindAStuckRenameOffTheCallersThread)
{
    auto drive = std::make_shared<ControlledDrive>();
    BackgroundRecordWriter writer (drive->function());

    writer.submit ("session.json", "refresh");
    drive->letCopy ("refresh");
    REQUIRE (drive->waitForRenaming ("refresh"));

    drive->letCopy ("stopped");
    drive->letRename ("stopped");

    const auto started = std::chrono::steady_clock::now();
    const auto written = writer.writeLast ("session.json", "stopped", std::chrono::milliseconds (150));
    REQUIRE (std::chrono::steady_clock::now() - started < std::chrono::seconds (2));
    REQUIRE_FALSE (written.has_value());

    // Still nothing in place: the stop-time record is behind the stuck rename.
    REQUIRE (drive->landedSoFar().empty());

    drive->letRename ("refresh");
    REQUIRE (drive->waitForLanded (2));

    const auto landed = drive->landedSoFar();
    REQUIRE (landed.size() == 2u);
    REQUIRE (landed[0] == "session.json=refresh");
    REQUIRE (landed[1] == "session.json=stopped");
}

// Whatever order a slow drive finishes things in, the stop-time record is the
// last thing put in place: many takes, each stopping with a refresh somewhere
// in the drive, the drive letting the two through in a random order.
TEST_CASE (BackgroundRecordWriter_TheLastRecordIsLastWhateverOrderTheDriveFinishesIn)
{
    std::mt19937 random (20261008u);

    for (int take = 0; take < 200; ++take)
    {
        auto drive = std::make_shared<ControlledDrive>();
        auto writer = std::make_unique<BackgroundRecordWriter> (drive->function());

        writer->submit ("session.json", "refresh");

        // Where the refresh is when Stop comes: queued, writing its copy, or
        // inside its rename.
        const auto stage = random() % 3;
        if (stage >= 1)
            REQUIRE (drive->waitForCopying ("refresh"));
        if (stage == 2)
        {
            drive->letCopy ("refresh");
            REQUIRE (drive->waitForRenaming ("refresh"));
        }

        const bool stoppedFirst = (random() % 2) == 0;
        if (stoppedFirst)
        {
            drive->letCopy ("stopped");
            drive->letRename ("stopped");
        }

        const auto written = writer->writeLast ("session.json", "stopped", std::chrono::milliseconds (random() % 3));
        writer.reset();

        if (! stoppedFirst)
        {
            drive->letCopy ("stopped");
            drive->letRename ("stopped");
        }

        std::this_thread::sleep_for (std::chrono::microseconds (random() % 500));
        drive->release();

        REQUIRE (drive->waitUntil ([&]
        {
            return ControlledDrive::contains (drive->landed, "session.json=stopped")
                && (! ControlledDrive::contains (drive->copying, "refresh")
                    || ControlledDrive::contains (drive->refused, "refresh")
                    || ControlledDrive::contains (drive->landed, "session.json=refresh"));
        }));

        const auto landed = drive->landedSoFar();
        REQUIRE (! landed.empty());
        REQUIRE (landed.back() == "session.json=stopped");
        (void) written;
    }
}

// The caller never waits on the drive, and a drive slower than the refresh
// rate gets the newest record, not a backlog.
TEST_CASE (BackgroundRecordWriter_SubmitNeverWaitsAndTheNewestWins)
{
    auto drive = std::make_shared<ControlledDrive>();
    BackgroundRecordWriter writer (drive->function());

    writer.submit ("a", "1");
    REQUIRE (drive->waitForCopying ("1"));

    const auto started = std::chrono::steady_clock::now();
    for (int i = 2; i <= 50; ++i)
        writer.submit ("a", std::to_string (i));
    REQUIRE (std::chrono::steady_clock::now() - started < std::chrono::milliseconds (500));

    drive->release();
    REQUIRE (drive->waitForLanded (2));
    REQUIRE (waitUntilIdle (writer));

    const auto landed = drive->landedSoFar();
    REQUIRE (landed.size() == 2u);
    REQUIRE (landed[0] == "a=1");
    REQUIRE (landed[1] == "a=50");
    REQUIRE (writer.retire (std::chrono::milliseconds (100)));
}

// The owner going away -- the take's writers replaced by the next take's, or
// the app quitting -- never waits on the drive, and nothing queued behind a
// stuck write, nor the stuck write itself once it reaches its rename, is put
// in place afterwards on its behalf.
TEST_CASE (BackgroundRecordWriter_GoingAwayNeverWaitsAndDropsWhatWasQueued)
{
    auto drive = std::make_shared<ControlledDrive>();
    auto writer = std::make_unique<BackgroundRecordWriter> (drive->function());

    writer->submit ("session.json", "first");
    REQUIRE (drive->waitForCopying ("first"));
    writer->submit ("session.json", "queued");

    const auto started = std::chrono::steady_clock::now();
    writer.reset();
    REQUIRE (std::chrono::steady_clock::now() - started < std::chrono::milliseconds (500));

    drive->release();

    // The worker outlives its owner, finishes the write it was in, and stops.
    REQUIRE (drive->waitForRefused ("first"));
    std::this_thread::sleep_for (std::chrono::milliseconds (100));

    const std::lock_guard<std::mutex> lock (drive->mutex);
    REQUIRE (drive->landed.empty());
    REQUIRE (drive->copying.size() == 1u);
}

// A drive that keeps up gets every refresh, in order, and the last one is the
// record left on it.
TEST_CASE (BackgroundRecordWriter_ADriveThatKeepsUpGetsEveryRefresh)
{
    std::atomic<int> writes { 0 };
    std::string landed;
    std::mutex landedMutex;

    BackgroundRecordWriter writer ([&] (const std::string&, const std::string& text,
                                        const BackgroundRecordWriter::Commit& commit)
    {
        return commit ([&]
        {
            const std::lock_guard<std::mutex> lock (landedMutex);
            landed = text;
            ++writes;
            return true;
        });
    });

    writer.submit ("session.json", "{\"dropouts\":[]}");
    REQUIRE (waitUntilIdle (writer));
    writer.submit ("session.json", "{\"dropouts\":[{\"device\":\"mic2\"}]}");
    REQUIRE (waitUntilIdle (writer));

    const auto written = writer.writeLast ("session.json", "{\"stop\":\"now\"}", std::chrono::milliseconds (2000));
    REQUIRE (written.has_value());
    REQUIRE (*written);

    const std::lock_guard<std::mutex> lock (landedMutex);
    REQUIRE (writes.load() == 3);
    REQUIRE (landed == "{\"stop\":\"now\"}");
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
