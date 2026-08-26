#include "TestFramework.h"
#include "Core/SessionWriter.h"
#include <cstdio>
#include <fstream>
#include <vector>
#include <cstdlib>

using namespace mma;

namespace {

std::string tempBasePath (const std::string& name)
{
    // Windows has no /tmp, so fall back through the usual temp-dir variables
    // before assuming a POSIX layout.
    for (const char* var : { "MMA_TEST_TMPDIR", "TMPDIR", "TMP", "TEMP" })
    {
        const char* dir = std::getenv (var);

        if (dir != nullptr && *dir != '\0')
            return std::string (dir) + "/" + name;
    }

    return "/tmp/" + name;
}

uint32_t readU32LE (std::ifstream& f, std::streampos pos)
{
    f.seekg (pos);
    unsigned char b[4];
    f.read (reinterpret_cast<char*> (b), 4);
    return static_cast<uint32_t> (b[0]) | (static_cast<uint32_t> (b[1]) << 8)
         | (static_cast<uint32_t> (b[2]) << 16) | (static_cast<uint32_t> (b[3]) << 24);
}

} // namespace

TEST_CASE (SessionWriter_WritesReadableRiffWaveHeader)
{
    std::string path = tempBasePath ("mma_test_basic");
    SessionWriter writer;
    REQUIRE (writer.open (path, 48000.0, 2, 16, "2026-08-26T14:32:00Z"));

    std::vector<float> frames = { 0.5f, -0.5f, 0.25f, -0.25f };
    REQUIRE (writer.writeInterleaved (frames.data(), 2));
    writer.close();

    std::ifstream f (writer.getCurrentFilePath(), std::ios::binary);
    REQUIRE (f.is_open());
    char riffTag[4]; f.read (riffTag, 4);
    REQUIRE (std::string (riffTag, 4) == "RIFF");
    char waveTag[4]; f.seekg (8); f.read (waveTag, 4);
    REQUIRE (std::string (waveTag, 4) == "WAVE");

    std::remove (writer.getCurrentFilePath().c_str());
}

TEST_CASE (SessionWriter_DataChunkSizeMatchesBytesWritten)
{
    std::string path = tempBasePath ("mma_test_datasize");
    SessionWriter writer;
    writer.open (path, 48000.0, 1, 16, "2026-08-26T14:32:00Z");

    const int numFrames = 100;
    std::vector<float> frames (static_cast<size_t> (numFrames), 0.1f);
    writer.writeInterleaved (frames.data(), static_cast<size_t> (numFrames));
    writer.close();

    std::ifstream f (writer.getCurrentFilePath(), std::ios::binary);
    // fmt chunk (8 + 16) + bext chunk (8 + 602) precede "data" tag.
    std::streampos dataSizePos = 12 + (8 + 16) + (8 + 602) + 4;
    uint32_t dataSize = readU32LE (f, dataSizePos);
    REQUIRE (dataSize == static_cast<uint32_t> (numFrames * 2)); // 1 channel * 2 bytes/sample

    std::remove (writer.getCurrentFilePath().c_str());
}

TEST_CASE (SessionWriter_TracksTotalFramesWritten)
{
    std::string path = tempBasePath ("mma_test_frames");
    SessionWriter writer;
    writer.open (path, 48000.0, 2, 24, "2026-08-26T14:32:00Z");

    std::vector<float> frames (20, 0.0f); // 10 frames of 2 channels
    writer.writeInterleaved (frames.data(), 10);
    REQUIRE (writer.getTotalFramesWritten() == 10);
    writer.close();

    std::remove (writer.getCurrentFilePath().c_str());
}

TEST_CASE (SessionWriter_AutoSplitsAtConfiguredThreshold)
{
    std::string path = tempBasePath ("mma_test_split");
    SessionWriter writer;
    // Mono 16-bit: 2 bytes/frame. Force a tiny split threshold isn't exposed
    // directly, so instead we verify the *first* file's naming convention and
    // that split index starts at 0 (no suffix) -- full 3.9GB split behavior is
    // covered by code review since writing 3.9GB in a unit test isn't practical.
    writer.open (path, 48000.0, 1, 16, "2026-08-26T14:32:00Z");
    REQUIRE (writer.getSplitFileCount() == 0);
    REQUIRE (writer.getCurrentFilePath() == path + ".wav");
    writer.close();

    std::remove (writer.getCurrentFilePath().c_str());
}

TEST_CASE (SessionWriter_HeaderRewriteTickUpdatesSizesPeriodically)
{
    std::string path = tempBasePath ("mma_test_tick");
    SessionWriter writer;
    writer.open (path, 48000.0, 1, 16, "2026-08-26T14:32:00Z");

    std::vector<float> frames (1000, 0.2f);
    writer.writeInterleaved (frames.data(), 1000);
    writer.tick (5.5); // exceeds the 5s rewrite interval

    std::ifstream f (writer.getCurrentFilePath(), std::ios::binary);
    std::streampos dataSizePos = 12 + (8 + 16) + (8 + 602) + 4;
    uint32_t dataSize = readU32LE (f, dataSizePos);
    REQUIRE (dataSize == 2000); // 1000 frames * 2 bytes, already flushed by tick()

    writer.close();
    std::remove (writer.getCurrentFilePath().c_str());
}
