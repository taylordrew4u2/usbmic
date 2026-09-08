#include "TestFramework.h"
#include "Core/RecordingProof.h"

using namespace mma;

namespace {
ProofReading at (double seconds, uint64_t frames, uint64_t bytes, float peak = 0.5f)
{
    ProofReading r;
    r.elapsedSeconds = seconds;
    r.framesAccepted = frames;
    r.bytesOnDisk = bytes;
    r.peakArrived = peak;
    return r;
}
constexpr uint64_t kHeaders = 7 * 650; // seven WAV headers, no audio
} // namespace

TEST_CASE (RecordingProof_ATakeThatNeverWritesIsStoppedAfterTheGrace)
{
    // The day-long take that produced seven empty files. Three seconds in,
    // the files are still headers: the take is stopped, not trusted.
    RecordingProof p;
    p.begin (at (0.0, 0, kHeaders));

    REQUIRE (p.observe (at (0.5, 0, kHeaders)) == ProofVerdict::TooEarly);
    REQUIRE (p.observe (at (2.9, 0, kHeaders)) == ProofVerdict::TooEarly);
    REQUIRE (p.observe (at (3.0, 0, kHeaders)) == ProofVerdict::NothingWritten);
}

TEST_CASE (RecordingProof_HeadersAloneDoNotCountAsGrowth)
{
    // Files that went from 0 bytes to a header each are still empty takes.
    RecordingProof p;
    p.begin (at (0.0, 0, 0));
    p.observe (at (1.0, 0, kHeaders));
    REQUIRE (p.observe (at (3.5, 0, kHeaders)) == ProofVerdict::NothingWritten);
}

TEST_CASE (RecordingProof_AGrowingTakeIsHealthy)
{
    RecordingProof p;
    p.begin (at (0.0, 0, kHeaders));
    const uint64_t perSecond = 7 * 48000 * 3;

    for (int s = 1; s <= 10; ++s)
        REQUIRE (p.observe (at (s, 48000ull * s, kHeaders + perSecond * s)) == (s < 3 ? ProofVerdict::TooEarly : ProofVerdict::Healthy));
}

TEST_CASE (RecordingProof_FilesThatStopGrowingAreAStall)
{
    RecordingProof p;
    p.begin (at (0.0, 0, kHeaders));
    const uint64_t perSecond = 7 * 48000 * 3;

    for (int s = 1; s <= 10; ++s)
        p.observe (at (s, 48000ull * s, kHeaders + perSecond * s));

    const uint64_t frozen = kHeaders + perSecond * 10;
    REQUIRE (p.observe (at (12.0, 48000ull * 12, frozen)) == ProofVerdict::Healthy);   // 2 s without growth: patience
    REQUIRE (p.observe (at (15.9, 48000ull * 15, frozen)) == ProofVerdict::Healthy);
    REQUIRE (p.observe (at (16.0, 48000ull * 16, frozen)) == ProofVerdict::Stalled);   // 6 s: the drive has gone
}

TEST_CASE (RecordingProof_SilenceIsSaidOnceAfterTwentySeconds)
{
    RecordingProof p;
    p.begin (at (0.0, 0, kHeaders, -1.0f));
    const uint64_t perSecond = 7 * 48000 * 3;

    for (int s = 1; s <= 19; ++s)
        REQUIRE (p.observe (at (s, 48000ull * s, kHeaders + perSecond * s, 0.0f)) != ProofVerdict::NoSoundArriving);

    REQUIRE (p.observe (at (20.0, 48000ull * 20, kHeaders + perSecond * 20, 0.0f)) == ProofVerdict::NoSoundArriving);
    REQUIRE (p.observe (at (21.0, 48000ull * 21, kHeaders + perSecond * 21, 0.0f)) == ProofVerdict::Healthy); // said once
}

TEST_CASE (RecordingProof_AQuietStartIsNotSilence)
{
    // Nobody spoke for the first ten seconds, then they did.
    RecordingProof p;
    p.begin (at (0.0, 0, kHeaders, -1.0f));
    const uint64_t perSecond = 7 * 48000 * 3;

    for (int s = 1; s <= 10; ++s)
        p.observe (at (s, 48000ull * s, kHeaders + perSecond * s, 0.0f));

    REQUIRE (p.observe (at (25.0, 48000ull * 25, kHeaders + perSecond * 25, 0.3f)) == ProofVerdict::Healthy);
}

TEST_CASE (RecordingProof_EveryVerdictThatMattersHasWordsForTheUser)
{
    REQUIRE (! RecordingProof::message (ProofVerdict::NothingWritten).empty());
    REQUIRE (! RecordingProof::message (ProofVerdict::Stalled).empty());
    REQUIRE (! RecordingProof::message (ProofVerdict::NoSoundArriving).empty());
    REQUIRE (RecordingProof::message (ProofVerdict::Healthy).empty());
    REQUIRE (RecordingProof::message (ProofVerdict::NothingWritten).find ("stopped") != std::string::npos);
}
