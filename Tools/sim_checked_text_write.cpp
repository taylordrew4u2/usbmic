// A full disk must never replace a good settings.json (or session.json, or a
// saved show) with an empty or cut-short one.
//
// The disk filling is simulated with RLIMIT_FSIZE: every write past the limit
// fails exactly as it does on a full volume (EFBIG instead of ENOSPC), and the
// rename that follows still succeeds -- which is the case that matters, since
// renaming the failed temporary file over the good one is the bug.
#include "App/CheckedTextWrite.h"
#include "Core/BackgroundRecordWriter.h"
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <sys/resource.h>

namespace {

int failures = 0;

void check (bool ok, const char* what)
{
    std::fprintf (ok ? stdout : stderr, "%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}

bool limitFileSize (rlim_t bytes)
{
    rlimit limit {};
    if (getrlimit (RLIMIT_FSIZE, &limit) != 0)
        return false;

    limit.rlim_cur = bytes;
    return setrlimit (RLIMIT_FSIZE, &limit) == 0;
}

bool unlimitFileSize()
{
    rlimit limit {};
    if (getrlimit (RLIMIT_FSIZE, &limit) != 0)
        return false;

    limit.rlim_cur = limit.rlim_max;
    return setrlimit (RLIMIT_FSIZE, &limit) == 0;
}

int countHiddenTemporaries (const juce::File& folder)
{
    int count = 0;
    for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, "*"))
        if (f.getFileName().startsWithChar ('.'))
            ++count;
    return count;
}

} // namespace

int main()
{
    // A write past RLIMIT_FSIZE raises SIGXFSZ, which would kill the process
    // instead of failing the write the way a full disk does.
    std::signal (SIGXFSZ, SIG_IGN);

    const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getNonexistentChildFile ("sim_checked_text_write", "", false);
    folder.createDirectory();

    const auto settings = folder.getChildFile ("settings.json");
    const juce::String good = "{\n  \"destinationFolder\": \"/Volumes/CARD\",\n  \"masterVolume\": 70\n}";

    check (mma::replaceWithTextChecked (settings, good), "a healthy write succeeds");
    check (settings.loadFileAsString() == good, "and the file holds exactly the text");

    // Bigger than JUCE's 16 KB stream buffer, so the failing write happens
    // while the text is going out, and again smaller than it, so it happens
    // when the buffer is flushed.
    const juce::String bigText = "{\"ports\": \"" + juce::String::repeatedString ("x", 64 * 1024) + "\"}";
    const juce::String smallText = "{\"ports\": \"" + juce::String::repeatedString ("y", 2000) + "\"}";

    for (const auto* text : { &bigText, &smallText })
    {
        // Room for a few bytes, nowhere near the whole file: the disk is full.
        check (limitFileSize (16), "simulated full disk installed");
        const bool reported = mma::replaceWithTextChecked (settings, *text);
        check (unlimitFileSize(), "simulated full disk removed");

        check (! reported, "a write that did not fit is reported as failed");
        check (settings.loadFileAsString() == good,
               "and the settings that were already there are untouched, not emptied");
        check (countHiddenTemporaries (folder) == 0, "and no half-written temporary file is left behind");
    }

    // The disk has room again: the next save goes through.
    check (mma::replaceWithTextChecked (settings, smallText), "a write after space is freed succeeds");
    check (settings.loadFileAsString() == smallText, "and replaces the file whole");

    // A file that does not exist yet and cannot be written must not appear
    // half-written either.
    const auto fresh = folder.getChildFile ("fresh.json");
    check (limitFileSize (16), "simulated full disk installed");
    const bool freshReported = mma::replaceWithTextChecked (fresh, bigText);
    check (unlimitFileSize(), "simulated full disk removed");
    check (! freshReported, "a new file that did not fit is reported as failed");
    check (! fresh.exists(), "and no cut-short file is left in its place");

    // A rename the caller declines (an older session.json refused because a
    // newer one is already in place) is a failure like any other: the file is
    // untouched and the temporary copy removed.
    const bool declined = mma::replaceWithTextChecked (settings, good, [] (const auto&) { return false; });
    check (! declined, "a declined rename is reported as not written");
    check (settings.loadFileAsString() == smallText, "and the file is left as it was");
    check (countHiddenTemporaries (folder) == 0, "and its temporary copy is removed");

    // The same on real files through BackgroundRecordWriter: a mid-take
    // refresh whose temporary copy is complete but whose rename is held up
    // (a slow backup drive) until after the stop-time record is in place is
    // refused at its rename, and session.json is the stop-time record.
    {
        const auto record = folder.getChildFile ("session.json");
        std::mutex gateMutex;
        std::condition_variable gateChanged;
        bool refreshReady = false, refreshReleased = false;

        mma::BackgroundRecordWriter writer ([&] (const std::string&, const std::string& text,
                                                 const mma::BackgroundRecordWriter::Commit& commit)
        {
            return mma::replaceWithTextChecked (record, juce::String (text), [&] (const auto& rename)
            {
                if (text == "refresh")
                {
                    std::unique_lock<std::mutex> lock (gateMutex);
                    refreshReady = true;
                    gateChanged.notify_all();
                    gateChanged.wait (lock, [&] { return refreshReleased; });
                }

                return commit (rename);
            });
        });

        writer.submit (record.getFullPathName().toStdString(), "refresh");
        {
            std::unique_lock<std::mutex> lock (gateMutex);
            gateChanged.wait_for (lock, std::chrono::seconds (5), [&] { return refreshReady; });
        }

        const auto stopped = writer.writeLast (record.getFullPathName().toStdString(), "stopped",
                                               std::chrono::milliseconds (5000));
        check (stopped.has_value() && *stopped, "the stop-time record is written");

        {
            const std::lock_guard<std::mutex> lock (gateMutex);
            refreshReleased = true;
            gateChanged.notify_all();
        }

        for (int i = 0; i < 500 && writer.isBusy(); ++i)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));

        check (! writer.isBusy(), "the held-up refresh finishes");
        check (record.loadFileAsString() == "stopped",
               "and session.json is still the stop-time record, not the refresh");
        check (countHiddenTemporaries (folder) == 0, "and the refused refresh leaves no temporary file");
    }

    folder.deleteRecursively();

    if (failures == 0)
        std::printf ("sim_checked_text_write: all checks passed\n");

    return failures == 0 ? 0 : 1;
}
