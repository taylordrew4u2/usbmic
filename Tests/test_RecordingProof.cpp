#include "TestFramework.h"
#include "Core/RecordingProof.h"
#include <string>
#include <vector>

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

ProofReading withoutDiskObservation (double seconds, uint64_t frames, float peak = 0.5f)
{
    auto reading = at (seconds, frames, 0, peak);
    reading.diskObservationAvailable = false;
    return reading;
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

TEST_CASE (RecordingProof_AnUnavailableAsyncSnapshotIsNotZeroBytes)
{
    RecordingProof p;
    p.begin (withoutDiskObservation (0.0, 0));

    // A slow or disappearing card can keep the worker from returning past the
    // three-second stop threshold. Missing evidence must not be turned into an
    // empty take while the audio path is still accepting frames.
    REQUIRE (p.observe (withoutDiskObservation (3.0, 144000)) == ProofVerdict::Healthy);
    REQUIRE (p.observe (withoutDiskObservation (8.0, 384000)) == ProofVerdict::Healthy);

    // Once a matching snapshot arrives, ordinary growth tracking resumes.
    REQUIRE (p.observe (at (9.0, 432000, kHeaders + 432000)) == ProofVerdict::Healthy);
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

// A stall is news once. The app acts on every Stalled verdict -- a new alert
// row, the card brought back, the siren -- and the verdict used to repeat on
// every half-second tick for as long as the drive stayed stuck, so pressing
// Keep recording brought the card and the siren straight back.
TEST_CASE (RecordingProof_AStallIsReportedOnceUntilTheFilesGrowAgain)
{
    RecordingProof p;
    p.begin (at (0.0, 0, kHeaders));
    const uint64_t perSecond = 7 * 48000 * 3;

    for (int s = 1; s <= 10; ++s)
        REQUIRE (p.observe (at (s, 48000ull * s, kHeaders + perSecond * s)) != ProofVerdict::Stalled);

    const uint64_t frozen = kHeaders + perSecond * 10;
    REQUIRE (p.observe (at (16.0, 48000ull * 16, frozen)) == ProofVerdict::Stalled);
    REQUIRE (p.observe (at (16.5, 48000ull * 16, frozen)) != ProofVerdict::Stalled);
    REQUIRE (p.observe (at (30.0, 48000ull * 30, frozen)) != ProofVerdict::Stalled);

    // The drive recovers, then sticks again: that is a second stall, and it
    // is reported.
    REQUIRE (p.observe (at (31.0, 48000ull * 31, frozen + perSecond)) != ProofVerdict::Stalled);
    REQUIRE (p.observe (at (37.0, 48000ull * 37, frozen + perSecond)) == ProofVerdict::Stalled);
    REQUIRE (p.observe (at (37.5, 48000ull * 37, frozen + perSecond)) != ProofVerdict::Stalled);
}

// The camera's movie sits in the take's folder beside the WAVs. Only the WAVs
// are the audio writer's work; a movie that keeps growing must not stand in
// for them.
TEST_CASE (RecordingProof_OnlyWavFilesCountAsRecordedAudio)
{
    REQUIRE (countsAsRecordedAudio ("MIX.wav"));
    REQUIRE (countsAsRecordedAudio ("01 Vocal.WAV"));
    REQUIRE (! countsAsRecordedAudio ("Camera.mov"));
    REQUIRE (! countsAsRecordedAudio ("Camera.mp4"));
    REQUIRE (! countsAsRecordedAudio ("session.json"));
    REQUIRE (! countsAsRecordedAudio ("wav"));
}

TEST_CASE (RecordingProof_AGrowingCameraMovieDoesNotHideAStalledAudioWriter)
{
    struct Entry { std::string name; uint64_t size; };
    const uint64_t perSecond = 48000ull * 3 * 2;
    const uint64_t moviePerSecond = 1'000'000;

    // The audio stops growing at 10 s; the camera keeps writing its movie.
    auto readingAt = [&] (double s)
    {
        const double audioSeconds = s < 10.0 ? s : 10.0;
        const std::vector<Entry> files {
            { "MIX.wav", kHeaders + static_cast<uint64_t> (perSecond * audioSeconds) },
            { "Camera.mov", static_cast<uint64_t> (moviePerSecond * s) },
        };
        uint64_t bytes = 0;
        for (const auto& f : files)
            if (countsAsRecordedAudio (f.name))
                bytes += f.size;
        return at (s, static_cast<uint64_t> (48000 * s), bytes);
    };

    RecordingProof p;
    p.begin (readingAt (0.0));
    for (int s = 1; s <= 15; ++s)
        REQUIRE (p.observe (readingAt (s)) != ProofVerdict::Stalled);
    REQUIRE (p.observe (readingAt (16.0)) == ProofVerdict::Stalled);
}
