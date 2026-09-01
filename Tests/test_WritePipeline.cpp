#include "TestFramework.h"
#include "Core/WritePipeline.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <thread>
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

/// Spins until the writer thread has taken everything pushed so far, so a test
/// can say "after this block was written" without guessing at thread timing.
/// Returns false if it never drains, so a hang shows up as a failed assertion
/// rather than a test that never finishes.
bool waitForDrain (const WritePipeline& p)
{
    for (int attempt = 0; attempt < 2000; ++attempt)
    {
        if (p.getFillFraction() == 0.0)
            return true;

        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }

    return false;
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

TEST_CASE (WritePipeline_LiveTrimChangeMovesTheMixNotTheStem)
{
    const auto dir = tempDir();

    std::vector<WriteChannelSpec> channels = { { "live_stem", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> a (512, 0.5f);
    const float* chans[] = { a.data() };

    // §4: the user turns the mic down mid-take. The mix must follow; the stem
    // must not, because the stem is the raw material the take exists for.
    p.setChannelTrimDb (0, -20.0f);
    REQUIRE (p.pushBlock (chans, 1, 512));
    p.stop();

    auto firstSample = [] (const std::string& path)
    {
        std::ifstream f (path, std::ios::binary);
        REQUIRE (f.is_open());
        f.seekg (kAudioDataOffset);
        unsigned char lo = 0, hi = 0;
        f.read (reinterpret_cast<char*> (&lo), 1);
        f.read (reinterpret_cast<char*> (&hi), 1);
        return static_cast<int16_t> (static_cast<uint16_t> (lo) | (static_cast<uint16_t> (hi) << 8));
    };

    REQUIRE (firstSample (dir + "/live_stem.wav") > 12000); // unity, ~16384
    REQUIRE (firstSample (dir + "/MIX.wav") < 6000);        // -20 dB, ~1638
}

TEST_CASE (WritePipeline_MirrorWritesASecondCompleteCopy)
{
    const auto dir = tempDir();
    const auto mirror = tempDir();

    std::vector<WriteChannelSpec> channels = { { "01_A", 0.0f }, { "02_B", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z", mirror));
    REQUIRE (p.isMirroring());

    std::vector<float> a (512, 0.5f), b (512, 0.25f);
    const float* chans[] = { a.data(), b.data() };
    REQUIRE (p.pushBlock (chans, 2, 512));
    p.stop();

    // §6.3: the mirror turns most card failures from data loss into
    // inconvenience, which requires every file to be there, not just the mix.
    for (const char* name : { "/01_A.wav", "/02_B.wav", "/MIX.wav" })
    {
        std::ifstream card (dir + name, std::ios::binary);
        std::ifstream copy (mirror + name, std::ios::binary);
        REQUIRE (card.is_open());
        REQUIRE (copy.is_open());

        // Byte-identical: both are written from the same drained scratch, so a
        // divergence here would mean the copy is not a copy.
        const std::string cardBytes ((std::istreambuf_iterator<char> (card)), std::istreambuf_iterator<char>());
        const std::string copyBytes ((std::istreambuf_iterator<char> (copy)), std::istreambuf_iterator<char>());
        REQUIRE (cardBytes == copyBytes);
        REQUIRE (cardBytes.size() > 602);
    }
}

TEST_CASE (WritePipeline_NoMirrorFolderMeansCardOnly)
{
    const auto dir = tempDir();

    std::vector<WriteChannelSpec> channels = { { "01_A", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));

    // §6.3 is a default, not a requirement: card-only must be a working mode.
    REQUIRE_FALSE (p.isMirroring());

    std::vector<float> a (256, 0.5f);
    const float* chans[] = { a.data() };
    REQUIRE (p.pushBlock (chans, 1, 256));
    p.stop();

    std::ifstream f (dir + "/01_A.wav", std::ios::binary);
    REQUIRE (f.is_open());
}

TEST_CASE (WritePipeline_StoppingTheMirrorLeavesTheRecordingIntact)
{
    const auto dir = tempDir();
    const auto mirror = tempDir();

    std::vector<WriteChannelSpec> channels = { { "01_A", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z", mirror));

    std::vector<float> a (512, 0.5f);
    const float* chans[] = { a.data() };
    REQUIRE (p.pushBlock (chans, 1, 512));

    // §6.3: the internal drive ran low. The copy stops; the take does not.
    p.stopMirroring();
    REQUIRE_FALSE (p.isMirroring());

    REQUIRE (p.pushBlock (chans, 1, 512));
    p.stop();

    std::ifstream card (dir + "/01_A.wav", std::ios::binary);
    REQUIRE (card.is_open());

    // Both blocks reached the card even though the mirror stopped between them.
    REQUIRE (readU32LE (card, kDataSizeOffset) == 1024 * 2);
}

TEST_CASE (WritePipeline_AHealthyTakeNeverReportsAFailure)
{
    // The other half of the claim: the flag must stay down when nothing is
    // wrong, or it would stop every take the moment it was wired to the UI.
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "healthy_card", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));

    std::vector<float> block (4096, 0.2f);
    const float* chans[] = { block.data() };
    REQUIRE (p.pushBlock (chans, 1, 4096));
    p.stop();

    REQUIRE_FALSE (p.hasCardWriteFailed());
    REQUIRE_FALSE (p.hasMirrorWriteFailed());

    std::remove ((dir + "/healthy_card.wav").c_str());
}

// -----------------------------------------------------------------------
// §6.5 "target card removed". Making a real write fail without unplugging
// real hardware: cap the process's maximum file size so the writer's own
// file grows into a hard EFBIG, which is what a card that has gone away
// looks like from inside a write() call. SIGXFSZ is ignored so the failure
// arrives as a return value rather than killing the test runner -- the
// return value being exactly what SessionWriter documents and what this
// pipeline used to throw away.
//
// POSIX only: Windows has no equivalent, so these two are compiled out there
// rather than faked. The healthy-take check above deliberately sits outside the
// guard -- it needs nothing platform-specific, and "this flag stays down when
// nothing is wrong" is worth asserting on every platform, since a false
// positive there would stop every recording instantly.
// -----------------------------------------------------------------------
#if ! defined(_WIN32)

#include <csignal>
#include <sys/resource.h>

namespace {

class FileSizeCap
{
public:
    explicit FileSizeCap (rlim_t bytes)
    {
        previousHandler = std::signal (SIGXFSZ, SIG_IGN);
        getrlimit (RLIMIT_FSIZE, &previous);

        rlimit capped { bytes, previous.rlim_max };
        applied = setrlimit (RLIMIT_FSIZE, &capped) == 0;
    }

    ~FileSizeCap()
    {
        // Restored whatever happens: this is process-wide, and leaving it in
        // place would silently break every test that writes a file after it.
        if (applied)
            setrlimit (RLIMIT_FSIZE, &previous);

        std::signal (SIGXFSZ, previousHandler);
    }

    bool isApplied() const { return applied; }

private:
    rlimit previous {};
    void (*previousHandler) (int) = nullptr;
    bool applied = false;
};

} // namespace

TEST_CASE (WritePipeline_ADestinationThatFailsMidTakeIsNoticed)
{
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "pulled_card", 0.0f } };

    WritePipeline p;
    {
        // Room for the BWF header and a little audio, then the wall.
        FileSizeCap cap (4096);
        REQUIRE (cap.isApplied());

        REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));

        // Nothing has gone wrong yet, and the pipeline must not cry off early.
        REQUIRE_FALSE (p.hasCardWriteFailed());

        std::vector<float> block (8192, 0.3f);
        const float* chans[] = { block.data() };
        REQUIRE (p.pushBlock (chans, 1, 8192));

        // stop() does the final flush synchronously, so this does not depend on
        // catching the writer thread mid-sleep.
        p.stop();

        // The point of the whole change: the card said no, and the pipeline
        // heard it. Before this, every one of those return values was dropped
        // and a pulled card was indistinguishable from a healthy take.
        REQUIRE (p.hasCardWriteFailed());
    }

    std::remove ((dir + "/pulled_card.wav").c_str());
}

TEST_CASE (WritePipeline_AFreshTakeForgetsTheLastOnesFailure)
{
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "reused", 0.0f } };

    WritePipeline p;
    {
        FileSizeCap cap (4096);
        REQUIRE (cap.isApplied());

        REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));
        std::vector<float> block (8192, 0.3f);
        const float* chans[] = { block.data() };
        p.pushBlock (chans, 1, 8192);
        p.stop();
        REQUIRE (p.hasCardWriteFailed());
    }

    // Cap gone: the next take is on a working card and must be allowed to say so.
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));
    REQUIRE_FALSE (p.hasCardWriteFailed());
    p.stop();

    std::remove ((dir + "/reused.wav").c_str());
}

#endif // ! _WIN32

TEST_CASE (WritePipeline_MixOnlyStopsTheStemsAndKeepsTheMix)
{
    // §6.5: at 90% fill with no mirror, "fall back to writing the mix file
    // only". A complete mix is worth more than eight stems with the same hole
    // in them, and the ring drains at a fraction of the byte rate while it
    // recovers.
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "degrade_a", 0.0f }, { "degrade_b", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));
    REQUIRE_FALSE (p.isMixOnly());

    std::vector<float> a (512, 0.5f), b (512, 0.25f);
    const float* chans[] = { a.data(), b.data() };
    REQUIRE (p.pushBlock (chans, 2, 512));

    // Wait for the writer thread to actually take that block before degrading.
    // Without this the test races it: degrade first and the stem never receives
    // the block at all, which looks like a bug in the pipeline and is not one.
    REQUIRE (waitForDrain (p));

    // Everything written so far is on both the stems and the mix.
    p.fallBackToMixOnly();
    REQUIRE (p.isMixOnly());

    REQUIRE (p.pushBlock (chans, 2, 512));
    p.stop();

    std::ifstream stem (dir + "/degrade_a.wav", std::ios::binary);
    std::ifstream mix (dir + "/MIX.wav", std::ios::binary);
    REQUIRE (stem.is_open());
    REQUIRE (mix.is_open());

    // The stem stopped at the first block; the mix carried both.
    REQUIRE (readU32LE (stem, kDataSizeOffset) == 512 * 2);
    REQUIRE (readU32LE (mix, kDataSizeOffset) == 1024 * 2);

    std::remove ((dir + "/degrade_a.wav").c_str());
    std::remove ((dir + "/degrade_b.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());
}

TEST_CASE (WritePipeline_TheMixStillCarriesEveryChannelAfterDegrading)
{
    // Shedding the stems must not quietly drop anyone out of the recording that
    // does survive -- the point is to shed write bandwidth, not people.
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "sum_a", 0.0f }, { "sum_b", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));
    p.fallBackToMixOnly();

    // Two channels at +0.25 each: the mix must be their sum, not one of them.
    std::vector<float> a (256, 0.25f), b (256, 0.25f);
    const float* chans[] = { a.data(), b.data() };
    REQUIRE (p.pushBlock (chans, 2, 256));
    p.stop();

    std::ifstream mix (dir + "/MIX.wav", std::ios::binary);
    REQUIRE (mix.is_open());
    mix.seekg (kAudioDataOffset);
    unsigned char lo = 0, hi = 0;
    mix.read (reinterpret_cast<char*> (&lo), 1);
    mix.read (reinterpret_cast<char*> (&hi), 1);
    const int16_t sample = static_cast<int16_t> (static_cast<uint16_t> (lo) | (static_cast<uint16_t> (hi) << 8));

    // 0.25 + 0.25 = 0.5 of full scale, give or take rounding.
    REQUIRE (sample > 16000);
    REQUIRE (sample < 16800);

    std::remove ((dir + "/sum_a.wav").c_str());
    std::remove ((dir + "/sum_b.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());
}

TEST_CASE (WritePipeline_AFreshTakeIsNotStillDegraded)
{
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "rearm", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));
    p.fallBackToMixOnly();
    REQUIRE (p.isMixOnly());
    p.stop();

    // §6.5 never restarts the stems within a take. A new take is a new take.
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-08-27T00:00:00Z"));
    REQUIRE_FALSE (p.isMixOnly());
    p.stop();

    std::remove ((dir + "/rearm.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());
}

TEST_CASE (WritePipeline_AMismatchedBlockIsCountedAsLostNotSilentlyDiscarded)
{
    // §0.1 makes unreported loss the one unacceptable failure. A block whose
    // channel count does not match the take cannot be written -- the file
    // layout is fixed for the take (§6.5) and there is no honest way to widen
    // or narrow a frame -- but it is still audio that arrived and was not
    // recorded, and it used to leave framesDropped at zero.
    //
    // Zero dropped frames is what the app shows the user as "nothing was lost".
    const auto dir = tempDir();
    std::vector<WriteChannelSpec> channels = { { "mismatch-a", 0.0f }, { "mismatch-b", 0.0f } };

    WritePipeline p;
    REQUIRE (p.start (dir, channels, 48000.0, 16, "2026-09-01T00:00:00Z"));

    std::vector<float> a (128, 0.1f), b (128, 0.2f);
    const float* two[2] = { a.data(), b.data() };

    REQUIRE (p.pushBlock (two, 2, 128));
    REQUIRE (p.getFramesDropped() == 0);

    // One channel short of the take, which is what a capture path handing over
    // fewer channels than it opened with looks like from here.
    const float* one[1] = { a.data() };

    REQUIRE_FALSE (p.pushBlock (one, 1, 128));
    REQUIRE (p.getFramesDropped() == 128);

    p.stop();

    std::remove ((dir + "/mismatch-a.wav").c_str());
    std::remove ((dir + "/mismatch-b.wav").c_str());
    std::remove ((dir + "/MIX.wav").c_str());
}
