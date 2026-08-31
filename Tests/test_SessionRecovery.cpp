#include "TestFramework.h"
#include "Core/SessionRecovery.h"
#include "Core/SessionWriter.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace mma;

namespace {

std::string tmpPath (const char* name)
{
    return std::string ("/tmp/mma-recovery-") + name;
}

/// Writes a real take with SessionWriter -- the same code that writes a real
/// recording -- then truncates the header's size fields back to what they would
/// have said at the last five-second rewrite before a crash.
std::string writeTakeThenStaleTheHeader (const char* name, double seconds, bool staleHeader)
{
    const auto base = tmpPath (name);
    const auto path = base + ".wav";
    std::remove (path.c_str());

    SessionWriter writer;
    REQUIRE (writer.open (base, 48000.0, 1, 24, "2026-08-31T05:00:00Z"));

    const int frames = static_cast<int> (48000.0 * seconds);
    std::vector<float> block (static_cast<size_t> (frames), 0.25f);
    REQUIRE (writer.writeInterleaved (block.data(), static_cast<size_t> (frames)));
    writer.close(); // a clean stop, which writes correct sizes

    if (staleHeader)
    {
        // Put the file back into the state a killed process leaves it in: the
        // audio is all there, the header still describes the file as it was at
        // the last rewrite. Zero is the extreme of that -- the placeholder the
        // writer starts with, which is what a crash in the first five seconds
        // leaves behind.
        std::fstream f (path, std::ios::in | std::ios::out | std::ios::binary);
        const char zero[4] = { 0, 0, 0, 0 };
        f.seekp (4);            // RIFF size
        f.write (zero, 4);
        f.close();

        // And the data size, wherever it landed after the bext chunk: found by
        // searching for the tag rather than assuming an offset.
        std::fstream g (path, std::ios::in | std::ios::out | std::ios::binary);
        std::string all ((std::istreambuf_iterator<char> (g)), std::istreambuf_iterator<char>());
        const auto at = all.find ("data");
        REQUIRE (at != std::string::npos);
        g.clear();
        g.seekp (static_cast<std::streamoff> (at) + 4);
        g.write (zero, 4);
        g.close();
    }

    return path;
}

uint32_t readU32At (const std::string& path, std::streamoff at)
{
    std::ifstream f (path, std::ios::binary);
    f.seekg (at);
    unsigned char b[4] = {};
    f.read (reinterpret_cast<char*> (b), 4);
    return static_cast<uint32_t> (b[0]) | (static_cast<uint32_t> (b[1]) << 8)
         | (static_cast<uint32_t> (b[2]) << 16) | (static_cast<uint32_t> (b[3]) << 24);
}

} // namespace

TEST_CASE (SessionRecovery_aTakeWithNoStopTimestampIsInterrupted)
{
    SessionMetadata meta;
    meta.startTimestampIso = "2026-08-31T05:00:00Z";
    REQUIRE (SessionRecovery::sessionWasInterrupted (meta));

    meta.stopTimestampIso = "2026-08-31T05:04:00Z";
    REQUIRE_FALSE (SessionRecovery::sessionWasInterrupted (meta));
}

TEST_CASE (SessionRecovery_aStaleHeaderIsRepairedFromTheBytesOnDisk)
{
    const auto path = writeTakeThenStaleTheHeader ("stale", 3.0, true);

    const auto result = SessionRecovery::repairWavFile (path);

    // The audio was always there; only the header disagreed. §6.6's five-second
    // rewrite exists to make exactly this recoverable.
    REQUIRE (result.headerWasStale);
    REQUIRE (result.frames == 48000u * 3u);
    REQUIRE (result.seconds > 2.99);
    REQUIRE (result.seconds < 3.01);
    REQUIRE_FALSE (result.reportedEmpty);

    // And it was actually written back, not merely reported.
    REQUIRE (readU32At (path, 4) != 0u);

    // Running it again finds nothing left to do, which is what makes it safe to
    // run over a whole card at launch.
    const auto second = SessionRecovery::repairWavFile (path);
    REQUIRE_FALSE (second.headerWasStale);
    REQUIRE (second.frames == result.frames);

    std::remove (path.c_str());
}

TEST_CASE (SessionRecovery_aCleanlyClosedFileIsLeftAlone)
{
    const auto path = writeTakeThenStaleTheHeader ("clean", 2.0, false);

    const auto result = SessionRecovery::repairWavFile (path);

    REQUIRE_FALSE (result.headerWasStale);
    REQUIRE (result.frames == 48000u * 2u);
    REQUIRE_FALSE (result.reportedEmpty);

    std::remove (path.c_str());
}

TEST_CASE (SessionRecovery_underASecondIsReportedEmptyRatherThanOffered)
{
    // §6.6: "discard any recovered file containing under 1 second of audio;
    // report it as empty rather than presenting an unplayable stub."
    const auto path = writeTakeThenStaleTheHeader ("stub", 0.5, true);

    const auto result = SessionRecovery::repairWavFile (path);

    REQUIRE (result.reportedEmpty);
    REQUIRE (result.frames == 24000u);

    // Left on disk rather than deleted: removing something from a user's card
    // at launch, unasked, is a worse mistake than listing a short file.
    std::ifstream still (path, std::ios::binary);
    REQUIRE (still.good());
    still.close();

    std::remove (path.c_str());
}

TEST_CASE (SessionRecovery_rubbishIsReportedEmptyRatherThanThrowing)
{
    // A folder pulled off a card can contain anything at all, and launch is the
    // worst possible moment to throw.
    const auto path = tmpPath ("rubbish.wav");
    {
        std::ofstream f (path, std::ios::binary);
        f << "this is not a wav file, not even slightly";
    }

    const auto result = SessionRecovery::repairWavFile (path);
    REQUIRE (result.frames == 0u);
    REQUIRE (result.reportedEmpty);
    REQUIRE (result.fileName == std::string ("mma-recovery-rubbish.wav"));

    std::remove (path.c_str());

    // A path that is not there at all is the same answer, not a crash.
    const auto missing = SessionRecovery::repairWavFile (tmpPath ("no-such-file.wav"));
    REQUIRE (missing.frames == 0u);
    REQUIRE (missing.reportedEmpty);
}

TEST_CASE (SessionRecovery_aSessionCountsWhatIsWorthOffering)
{
    RecoveredSession session;
    session.folder = "/RECORDINGS/2026-08-31_0500_Session";
    session.files.push_back ({ "MIX.wav", 48000 * 4, 4.0, true, false });
    session.files.push_back ({ "01_Alice.wav", 48000 * 4, 4.0, true, false });
    session.files.push_back ({ "02_Bob.wav", 100, 0.002, true, true });

    REQUIRE (session.keptFileCount() == 2);
    REQUIRE (session.emptyFileCount() == 1);
    REQUIRE (session.longestSeconds() == 4.0);
    REQUIRE (session.isWorthPresenting());

    // A take where nothing survived is not presented at all: §6.6 would rather
    // say nothing than hand someone an unplayable stub.
    RecoveredSession allEmpty;
    allEmpty.files.push_back ({ "MIX.wav", 10, 0.0002, true, true });
    REQUIRE_FALSE (allEmpty.isWorthPresenting());
    REQUIRE (allEmpty.longestSeconds() == 0.0);
}
