#include "TestFramework.h"
#include "Core/SessionRecovery.h"
#include "Core/SessionWriter.h"
#include "Core/Utf8Path.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace mma;

namespace {

std::string tmpPath (const char* name)
{
    // Windows has no /tmp, so fall back through the usual temp-dir variables
    // before assuming a POSIX layout -- the same reasoning, and the same list,
    // as tempBasePath() in test_SessionWriter.cpp.
    for (const char* var : { "MMA_TEST_TMPDIR", "TMPDIR", "TMP", "TEMP" })
    {
        const char* dir = std::getenv (var);

        if (dir != nullptr && *dir != '\0')
            return std::string (dir) + "/mma-recovery-" + name;
    }

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
    REQUIRE (result.fileName.find ('/') == std::string::npos);

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

TEST_CASE (SessionRecovery_AFileThatIsThereButUnreadableIsNotCalledEmpty)
{
    // A path with nothing at it is nothing, and "empty" is the right answer --
    // that is the case above. A file that IS there and cannot be opened for
    // repair is a recording of unknown length on a card that has gone
    // read-only, and calling THAT empty sends the user away from audio that may
    // be perfectly intact. The two used to give the same answer.
    //
    // Driven through the read-only case by pointing at a directory: it exists,
    // so the existence check passes, and it cannot be opened as a file.
    // std::filesystem rather than mkdir: these tests run on Windows CI too.
    const auto dir = tmpPath ("mma-recovery-dir-case");
    std::filesystem::create_directory (dir);

    const auto result = SessionRecovery::repairWavFile (dir);

    REQUIRE_FALSE (result.reportedEmpty);
    REQUIRE (result.repairFailed);

    std::filesystem::remove (dir);
}

TEST_CASE (SessionRecovery_MalformedChunksCannotLoopOrReadOutsideTheirBounds)
{
    // Includes the exact overflow that used to seek back to the fmt header.
    for (const uint32_t size : { 0xfffffff8u, 0xffffffffu, 0u, 15u, 100u })
    {
        const auto path = tmpPath ("bad-chunk.wav");
        std::ofstream f (path, std::ios::binary);
        const auto u32 = [&f] (uint32_t value) {
            for (int i = 0; i < 4; ++i) f.put (static_cast<char> (value >> (8 * i)));
        };
        f.write ("RIFF", 4); u32 (28); f.write ("WAVEfmt ", 8); u32 (size);
        const char body[16] = { 1, 0, 1, 0, char(0x80), char(0xbb), 0, 0,
                               0, char(0x77), 1, 0, 2, 0, 16, 0 };
        f.write (body, 16); f.close();
        const auto before = std::filesystem::file_size (path);
        const auto result = SessionRecovery::repairWavFile (path);
        REQUIRE (result.frames == 0);
        REQUIRE_FALSE (result.headerWasStale);
        REQUIRE (std::filesystem::file_size (path) == before);
        std::remove (path.c_str());
    }
}

TEST_CASE (SessionRecovery_OddExtendedFmtAndUnknownChunksKeepTheirPadding)
{
    const auto path = tmpPath ("padded-chunks.wav");
    std::ofstream f (path, std::ios::binary);
    const auto u32 = [&f] (uint32_t value) {
        for (int i = 0; i < 4; ++i) f.put (static_cast<char> (value >> (8 * i)));
    };
    f.write ("RIFF", 4); u32 (0); f.write ("WAVEJUNK", 8); u32 (1);
    f.put ('x'); f.put (0);
    f.write ("fmt ", 4); u32 (17);
    const char body[16] = { 1, 0, 1, 0, 2, 0, 0, 0, 4, 0, 0, 0, 2, 0, 16, 0 };
    f.write (body, 16); f.put (0); f.put (0);
    f.write ("data", 4); u32 (0); f.write ("abcd", 4); f.close();
    const auto result = SessionRecovery::repairWavFile (path);
    REQUIRE (result.frames == 2);
    REQUIRE (result.headerWasStale);
    REQUIRE_FALSE (result.reportedEmpty);
    std::remove (path.c_str());
}

TEST_CASE (RecoveredSession_APlayableCountExcludesWhatCouldNotBeRepaired)
{
    // The recovery headline announced session.files.size() -- every file in the
    // folder -- as "repaired and can be played", directly under a warning
    // saying a particular file could not be opened. keptFileCount() is no help
    // either: a failed repair deliberately sets reportedEmpty = false, so it
    // counts as kept.
    RecoveredSession s;

    RecoveredFile good;
    good.fileName = "01_Alex.wav";

    RecoveredFile empty;
    empty.fileName = "02_Sam.wav";
    empty.reportedEmpty = true;

    RecoveredFile broken;
    broken.fileName = "03_Jo.wav";
    broken.repairFailed = true;   // reportedEmpty stays false, deliberately

    s.files = { good, empty, broken };

    REQUIRE (s.files.size() == 3);
    REQUIRE (s.keptFileCount() == 2);      // the broken one still counts here
    REQUIRE (s.playableFileCount() == 1);  // and must not count here
    REQUIRE (s.emptyFileCount() == 1);
}

TEST_CASE (Utf8Path_aNonAsciiFolderNameSurvivesTheRoundTrip)
{
    // Every path in Core is a UTF-8 std::string. Handed straight to fstream or
    // std::filesystem, MSVC reads it in the ANSI code page, so "Zo\xC3\xAB"
    // becomes "ZoÃ«" -- a different folder, which does not exist. The helper is
    // the one place that says "these bytes are UTF-8".
    const std::string name = "Zo\xC3\xAB";
    REQUIRE (utf8FromPath (pathFromUtf8 (name)) == name);
}

TEST_CASE (SessionRecovery_aTakeUnderANonAsciiFolderRecordsAndRepairs)
{
    // A user profile or card label with an accent in it -- C:\Users\Zoë --
    // made every take fail to start on Windows: the writer could not create
    // its first file, and recovery could not open what was there. Passes on
    // Linux either way (narrow paths are already UTF-8 there); on the Windows
    // CI job it fails without the u8path conversions in SessionWriter and
    // SessionRecovery.
    const auto folder = tmpPath ("Zo\xC3\xAB-take");
    std::error_code ec;
    std::filesystem::remove_all (pathFromUtf8 (folder), ec);
    REQUIRE (std::filesystem::create_directories (pathFromUtf8 (folder), ec));

    const auto base = folder + "/01_Zo\xC3\xAB";
    const auto path = base + ".wav";

    {
        SessionWriter writer;
        REQUIRE (writer.open (base, 48000.0, 1, 24, "2026-08-31T05:00:00Z"));

        std::vector<float> block (48000u * 2u, 0.25f);
        REQUIRE (writer.writeInterleaved (block.data(), block.size()));
        writer.close();
    }

    // The file landed under the name it was given, not a code-page mangling
    // of it beside the folder.
    REQUIRE (std::filesystem::exists (pathFromUtf8 (path), ec));

    {
        // Stale both size fields, as a killed process would leave them.
        std::fstream f (pathFromUtf8 (path), std::ios::in | std::ios::out | std::ios::binary);
        std::string all ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
        const auto at = all.find ("data");
        REQUIRE (at != std::string::npos);
        const char zero[4] = { 0, 0, 0, 0 };
        f.clear();
        f.seekp (4);
        f.write (zero, 4);
        f.seekp (static_cast<std::streamoff> (at) + 4);
        f.write (zero, 4);
    }

    const auto result = SessionRecovery::repairWavFile (path);
    REQUIRE (result.headerWasStale);
    REQUIRE_FALSE (result.repairFailed);
    REQUIRE_FALSE (result.reportedEmpty);
    REQUIRE (result.frames == 48000u * 2u);
    REQUIRE (result.fileName == "01_Zo\xC3\xAB.wav");

    std::filesystem::remove_all (pathFromUtf8 (folder), ec);
}

TEST_CASE (SessionRecovery_AHiddenBackupCopyIsRememberedSoItCanBeDismissedToo)
{
    const auto take = [] (const std::string& folder)
    {
        RecoveredSession s;
        s.folder = folder;
        s.files.push_back ({ "MIX.wav", 48000 * 4, 4.0, true, false });
        return s;
    };

    // Either scan can finish first; the card copy wins both ways, and the
    // backup's folder is kept so dismissing the card stamps it as well.
    for (const bool mirrorFirst : { true, false })
    {
        RecoveredSessionList list;
        const std::vector<RecoveredSession> mirror { take ("/Users/me/RECORDINGS-MIRROR/2026-09-01_2000_Take"),
                                                     take ("/Users/me/RECORDINGS-MIRROR/2026-09-01_2100_Other") };
        const std::vector<RecoveredSession> card { take ("/Volumes/CARD/RECORDINGS/2026-09-01_2000_Take") };

        if (mirrorFirst)
            list = SessionRecovery::mergeScan (SessionRecovery::mergeScan (std::move (list), mirror, false),
                                               card, true);
        else
            list = SessionRecovery::mergeScan (SessionRecovery::mergeScan (std::move (list), card, true),
                                               mirror, false);

        REQUIRE (list.shown.size() == 2);
        const auto shownTake = std::find_if (list.shown.begin(), list.shown.end(),
                                             [] (const RecoveredSession& s)
                                             { return s.folder.find ("2000_Take") != std::string::npos; });
        REQUIRE (shownTake != list.shown.end());
        REQUIRE (shownTake->folder == "/Volumes/CARD/RECORDINGS/2026-09-01_2000_Take");
        REQUIRE (list.hiddenFolders.size() == 1);
        REQUIRE (list.hiddenFolders[0] == "/Users/me/RECORDINGS-MIRROR/2026-09-01_2000_Take");
    }
}

TEST_CASE (SessionRecovery_anOddLengthDataChunkIsRepairedWithItsPadByte)
{
    // Mono 24-bit is three bytes a frame, so an odd number of frames is an odd
    // data chunk -- and RIFF chunks are word-aligned, so it needs a pad byte
    // after it that the RIFF size counts. Built by hand as the placeholder a
    // crash in the first five seconds leaves: both sizes still zero.
    const auto path = tmpPath ("odd-data.wav");
    std::remove (path.c_str());

    {
        std::ofstream f (path, std::ios::binary);
        const auto u32 = [&f] (uint32_t v)
        {
            const char b[4] = { static_cast<char> (v & 0xFF), static_cast<char> ((v >> 8) & 0xFF),
                                static_cast<char> ((v >> 16) & 0xFF), static_cast<char> ((v >> 24) & 0xFF) };
            f.write (b, 4);
        };
        const auto u16 = [&f] (uint16_t v)
        {
            const char b[2] = { static_cast<char> (v & 0xFF), static_cast<char> ((v >> 8) & 0xFF) };
            f.write (b, 2);
        };

        f.write ("RIFF", 4); u32 (0); f.write ("WAVE", 4);
        f.write ("fmt ", 4); u32 (16);
        u16 (1); u16 (1); u32 (48000); u32 (48000 * 3); u16 (3); u16 (24);
        f.write ("data", 4); u32 (0);
        const char frames[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        f.write (frames, 9);
    }

    constexpr std::streamoff dataSizeAt = 40;

    const auto result = SessionRecovery::repairWavFile (path);
    REQUIRE (result.headerWasStale);
    REQUIRE_FALSE (result.repairFailed);
    REQUIRE (result.frames == 3u);

    const auto size = static_cast<uint32_t> (std::filesystem::file_size (path));
    REQUIRE (size % 2 == 0);
    REQUIRE (readU32At (path, 4) == size - 8);
    REQUIRE (readU32At (path, dataSizeAt) == 9u);

    // Still safe to run twice: the pad byte is not mistaken for audio.
    const auto second = SessionRecovery::repairWavFile (path);
    REQUIRE_FALSE (second.headerWasStale);
    REQUIRE (second.frames == 3u);
    REQUIRE (std::filesystem::file_size (path) == size);

    std::remove (path.c_str());
}

TEST_CASE (SessionRecovery_theCardDoesNotCallUnrepairedFilesPlayable)
{
    // A card that went read-only took the repair on one file and dropped it on
    // the other. The card used to say the take "has been repaired and is
    // playable" either way.
    RecoveredSession session;
    session.folder = "/Volumes/CARD/RECORDINGS/2026-09-01_2000_Take";
    session.files.push_back ({ "MIX.wav", 48000 * 4, 4.0, true, false, false });
    session.files.push_back ({ "VOX.wav", 48000 * 4, 4.0, true, false, true });

    const auto row = recoveredTakeRow (session);
    REQUIRE (row.folderName == "2026-09-01_2000_Take");
    REQUIRE (row.fileCount == 2);
    REQUIRE (row.playableFileCount == 1);

    const auto explanation = recoveredTakesExplanation ({ row });
    REQUIRE (explanation.find ("is playable") == std::string::npos);
    REQUIRE (explanation.find ("couldn't be repaired") != std::string::npos);
    REQUIRE (recoveredTakeDetail (row) == "2 files, 4s of sound, 1 couldn't be repaired");

    // A take the repair fully reached still says so.
    session.files[1].repairFailed = false;
    const auto good = recoveredTakeRow (session);
    REQUIRE (recoveredTakesExplanation ({ good }).find ("repaired and is playable") != std::string::npos);
    REQUIRE (recoveredTakeDetail (good) == "2 files, 4s of sound");
}
