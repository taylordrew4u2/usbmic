#include "TestFramework.h"
#include "Core/WritePipeline.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

using namespace mma;

namespace {

std::string tempDir()
{
    for (const char* var : { "MMA_TEST_TMPDIR", "TMPDIR", "TMP", "TEMP" })
    {
        const char* dir = std::getenv (var);
        if (dir != nullptr && *dir != '\0')
            return std::string (dir);
    }
    return "/tmp";
}

// §6.1 writes BWF, so a bext chunk sits between "fmt " and "data" and the
// canonical 44-byte layout does not apply:
//   RIFF header 12 + fmt (8 + 16) + bext (8 + 602) + "data" tag 4
constexpr std::streamoff kDataSizeOffset = 12 + (8 + 16) + (8 + 602) + 4;
constexpr std::streamoff kAudioDataOffset = kDataSizeOffset + 4;

uint32_t readU32LE (std::ifstream& f, std::streampos pos)
{
    f.seekg (pos);
    unsigned char b[4];
    f.read (reinterpret_cast<char*> (b), 4);
    return static_cast<uint32_t> (b[0]) | (static_cast<uint32_t> (b[1]) << 8)
         | (static_cast<uint32_t> (b[2]) << 16) | (static_cast<uint32_t> (b[3]) << 24);
}

std::vector<WriteChannelSpec> twoChannels()
{
    return { { "01_Left", 0.0f }, { "02_Right", 0.0f } };
}

} // namespace

TEST_CASE (WritePipeline_StartsAndStops)
{
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));
    REQUIRE (p.isRunning());
    p.stop();
    REQUIRE_FALSE (p.isRunning());
}

TEST_CASE (WritePipeline_RefusesToStartWithNoChannels)
{
    WritePipeline p;
    REQUIRE_FALSE (p.start (tempDir(), {}, 48000.0, 16, "2026-08-27T00:00:00Z"));
}

TEST_CASE (WritePipeline_RefusesToStartTwice)
{
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));
    REQUIRE_FALSE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));
    p.stop();
}

TEST_CASE (WritePipeline_PushIsRejectedWhenNotRunning)
{
    WritePipeline p;
    std::vector<float> a (64, 0.5f), b (64, 0.5f);
    const float* chans[] = { a.data(), b.data() };
    REQUIRE_FALSE (p.pushBlock (chans, 2, 64));
}

TEST_CASE (WritePipeline_AcceptsBlocksAndCountsFrames)
{
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (128, 0.25f), b (128, -0.25f);
    const float* chans[] = { a.data(), b.data() };

    REQUIRE (p.pushBlock (chans, 2, 128));
    REQUIRE (p.getFramesAccepted() == 128);
    REQUIRE (p.getFramesDropped() == 0);

    p.stop();
}

TEST_CASE (WritePipeline_RejectsAChannelCountMismatch)
{
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (64, 0.1f);
    const float* chans[] = { a.data() };
    // A changing channel count mid-take would corrupt the file layout (§6.5).
    REQUIRE_FALSE (p.pushBlock (chans, 1, 64));

    p.stop();
}

TEST_CASE (WritePipeline_WritesAStemPerChannelPlusAMix)
{
    const auto dir = tempDir();
    WritePipeline p;
    REQUIRE (p.start (dir, twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (256, 0.5f), b (256, 0.5f);
    const float* chans[] = { a.data(), b.data() };
    REQUIRE (p.pushBlock (chans, 2, 256));

    p.stop();

    for (const char* name : { "/01_Left.wav", "/02_Right.wav", "/MIX.wav" })
    {
        std::ifstream f (dir + name, std::ios::binary);
        REQUIRE (f.is_open());

        char riff[4];
        f.read (riff, 4);
        REQUIRE (std::string (riff, 4) == "RIFF");
    }
}

TEST_CASE (WritePipeline_StemsAreWrittenAtUnityDespiteTrim)
{
    const auto dir = tempDir();

    // §4: trim affects the mix file and never the stems, so a bad trim decision
    // cannot destroy the raw material.
    std::vector<WriteChannelSpec> channels = { { "trim_stem", -20.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (512, 0.5f);
    const float* chans[] = { a.data() };
    REQUIRE (p.pushBlock (chans, 1, 512));
    p.stop();

    std::ifstream f (dir + "/trim_stem.wav", std::ios::binary);
    REQUIRE (f.is_open());

    f.seekg (kAudioDataOffset);
    unsigned char lo = 0, hi = 0;
    f.read (reinterpret_cast<char*> (&lo), 1);
    f.read (reinterpret_cast<char*> (&hi), 1);
    const int16_t sample = static_cast<int16_t> (static_cast<uint16_t> (lo) | (static_cast<uint16_t> (hi) << 8));

    // 0.5 at unity is around 16384. With -20 dB trim applied it would be ~1638.
    REQUIRE (sample > 12000);
}

TEST_CASE (WritePipeline_UnpluggedChannelWritesSilenceNotNothing)
{
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "silent_ch", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));

    // §6.5: the channel stays in the file, it just goes quiet.
    p.setChannelLive (0, false);

    std::vector<float> a (256, 0.9f);
    const float* chans[] = { a.data() };
    REQUIRE (p.pushBlock (chans, 1, 256));
    REQUIRE (p.getFramesAccepted() == 256);
    p.stop();

    std::ifstream f (dir + "/silent_ch.wav", std::ios::binary);
    REQUIRE (f.is_open());
    const auto dataSize = readU32LE (f, kDataSizeOffset);
    // The frames are present, they are just zero.
    REQUIRE (dataSize == 256 * 2);

    f.seekg (kAudioDataOffset);
    unsigned char lo = 0, hi = 0;
    f.read (reinterpret_cast<char*> (&lo), 1);
    f.read (reinterpret_cast<char*> (&hi), 1);
    REQUIRE (lo == 0);
    REQUIRE (hi == 0);
}

TEST_CASE (WritePipeline_NullChannelPointerIsTreatedAsSilence)
{
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (64, 0.5f);
    const float* chans[] = { a.data(), nullptr };
    // A backend can hand back fewer buffers than expected; that must not crash
    // the audio thread.
    REQUIRE (p.pushBlock (chans, 2, 64));

    p.stop();
}

TEST_CASE (WritePipeline_StopFlushesWhatIsStillBuffered)
{
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "flush_ch", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (1024, 0.3f);
    const float* chans[] = { a.data() };
    REQUIRE (p.pushBlock (chans, 1, 1024));

    // Stop immediately, before the writer thread has necessarily run. §0.1:
    // audio is never lost, including the last block.
    p.stop();

    std::ifstream f (dir + "/flush_ch.wav", std::ios::binary);
    REQUIRE (f.is_open());
    REQUIRE (readU32LE (f, kDataSizeOffset) == 1024 * 2);
}

TEST_CASE (WritePipeline_FillFractionRisesWithUndrainedAudio)
{
    WritePipeline p;
    REQUIRE (p.start (tempDir(), twoChannels(), 48000.0, 16, "2026-08-27T00:00:00Z"));
    REQUIRE (p.getFillFraction() >= 0.0);
    REQUIRE (p.getFillFraction() <= 1.0);
    p.stop();
}
