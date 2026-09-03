#include "TestFramework.h"
#include "Core/LoudnessMeter.h"
#include "Core/StreamingTargets.h"
#include <cmath>
#include <vector>

using namespace mma;

namespace {

constexpr double kRate = 48000.0;

// Not M_PI -- see the note in LoudnessMeter.cpp. MSVC does not define it from
// <cmath>, and this file is compiled on Windows too.
constexpr double kTwoPi = 6.283185307179586476925286766559;

/// A sine at a given amplitude, long enough for the gated measurement to have
/// plenty of 400 ms blocks to work with.
std::vector<float> sine (double seconds, double amplitude, double frequency = 1000.0)
{
    const auto count = static_cast<size_t> (kRate * seconds);
    std::vector<float> out (count);

    for (size_t i = 0; i < count; ++i)
        out[i] = static_cast<float> (amplitude
                                     * std::sin (kTwoPi * frequency
                                                 * static_cast<double> (i) / kRate));

    return out;
}

double measure (const std::vector<float>& samples)
{
    LoudnessMeter meter (kRate);
    meter.process (samples.data(), samples.size());
    return meter.getIntegratedLufs();
}

} // namespace

TEST_CASE (Loudness_DoublingAmplitudeIsSixDecibelsLouder)
{
    // Exact, and independent of whether the K-weighting coefficients are right:
    // twice the amplitude is 6.02 dB whatever filter it passes through, because
    // the filter is linear. If this fails, the meter is not measuring power.
    const double quiet = measure (sine (5.0, 0.1));
    const double loud  = measure (sine (5.0, 0.2));

    REQUIRE (std::abs ((loud - quiet) - 6.02) < 0.05);
}

TEST_CASE (Loudness_MatchesTheStandardsOwnCalibration)
{
    // BS.1770's calibration: a 1 kHz sine at -20 dBFS on a single channel
    // measures -23.0 LKFS. That number is the whole reason the -0.691 offset is
    // in the loudness equation, so it checks the offset, the channel weight and
    // the K-weighting gain at 1 kHz all at once.
    //
    // -20 dBFS as an amplitude is 0.1.
    const double measured = measure (sine (5.0, 0.1));

    REQUIRE (std::abs (measured - (-23.0)) < 0.35);
}

TEST_CASE (Loudness_SilenceBetweenSentencesDoesNotDragTheFigureDown)
{
    // The gates are the point of BS.1770. Ungated, a take of someone talking
    // with pauses measures quieter than the same voice without them -- so the
    // number would say "turn it up" for nothing more than leaving space to
    // breathe, and following it would make the speech too loud.
    auto speech = sine (5.0, 0.1);
    const double withoutPauses = measure (speech);

    // The same speech with four seconds of silence spliced onto it.
    auto withPauses = speech;
    withPauses.insert (withPauses.end(), static_cast<size_t> (kRate * 4.0), 0.0f);

    const double gated = measure (withPauses);

    REQUIRE (std::abs (gated - withoutPauses) < 0.5);
}

TEST_CASE (Loudness_TrueSilenceIsReportedAsSilenceNotAsAQuietNumber)
{
    std::vector<float> quiet (static_cast<size_t> (kRate * 3.0), 0.0f);

    // Not "very quiet" -- nothing. A number here would be advised on, and the
    // advice would be to turn a silent take up by 180 dB.
    REQUIRE (measure (quiet) <= LoudnessMeter::kAbsoluteGateLufs);
}

TEST_CASE (Loudness_TruePeakSeesBetweenTheSamples)
{
    // A waveform can pass between two samples at a level higher than either, so
    // a file whose sample peak reads -1 dBFS can still clip a platform's
    // decoder. This is the case that shows the difference: a sine placed so its
    // crests land between sample points.
    LoudnessMeter meter (kRate);
    const auto samples = sine (1.0, 0.99, 11025.0); // high enough that crests fall between samples
    meter.process (samples.data(), samples.size());

    REQUIRE (meter.getTruePeakDbtp() >= meter.getSamplePeakDbfs());
}

TEST_CASE (Loudness_AShortTakeIsNotJudged)
{
    LoudnessMeter meter (kRate);
    const auto samples = sine (0.5, 0.1);
    meter.process (samples.data(), samples.size());

    const auto* spotify = findStreamingTarget ("Spotify");
    REQUIRE (spotify != nullptr);

    const auto advice = adviseForTarget (*spotify, meter.getIntegratedLufs(),
                                         meter.getTruePeakDbtp(), meter.getBlockCount());

    REQUIRE_FALSE (advice.measurable);
    REQUIRE_FALSE (advice.summary.empty());
}

TEST_CASE (StreamingTargets_MonoAimsThreeBelowThePublishedFigure)
{
    // The correction this app most needs and is easiest to get wrong: every
    // file it writes is mono, and a mono file played through both speakers
    // measures 3 LU louder than the single channel. Delivered at the published
    // -16, it plays back at -13 -- three decibels hotter than everything around
    // it, on every platform, every time.
    const auto* apple = findStreamingTarget ("Apple Podcasts");
    REQUIRE (apple != nullptr);
    REQUIRE (apple->stereoTargetLufs == -16.0);

    REQUIRE (std::abs (monoTargetLufs (*apple) - (-19.01)) < 0.02);

    const auto* spotify = findStreamingTarget ("Spotify");
    REQUIRE (spotify != nullptr);
    REQUIRE (std::abs (monoTargetLufs (*spotify) - (-17.01)) < 0.02);
}

TEST_CASE (StreamingTargets_TellsYouWhichWayToGoAndByHowMuch)
{
    const auto* spotify = findStreamingTarget ("Spotify");
    REQUIRE (spotify != nullptr);

    // Ten decibels under the mono aim of -17, with peaks far from the ceiling.
    const auto advice = adviseForTarget (*spotify, -27.0, -20.0, 100);

    REQUIRE (advice.measurable);
    REQUIRE_FALSE (advice.alreadyOnTarget);
    REQUIRE (std::abs (advice.gainDb - 10.0) < 0.05);
    REQUIRE_FALSE (advice.limitedByTruePeak);
}

TEST_CASE (StreamingTargets_NeverAdvisesGainThatWouldClip)
{
    const auto* spotify = findStreamingTarget ("Spotify");
    REQUIRE (spotify != nullptr);

    // Well under the target, but already peaking at -2 dBTP: there is only 1 dB
    // of room before the platform's ceiling. Meeting the loudness figure by
    // clipping trades a number the platform would have fixed anyway for
    // distortion it cannot.
    const auto advice = adviseForTarget (*spotify, -27.0, -2.0, 100);

    REQUIRE (advice.measurable);
    REQUIRE (advice.limitedByTruePeak);
    REQUIRE (std::abs (advice.gainDb - 1.0) < 0.05);
}

TEST_CASE (StreamingTargets_CloseEnoughIsLeftAlone)
{
    const auto* spotify = findStreamingTarget ("Spotify");
    REQUIRE (spotify != nullptr);

    // A quarter of a decibel from the mono aim. Below what anyone can hear on a
    // voice, and chasing it would have the advice change every time someone
    // cleared their throat.
    const auto advice = adviseForTarget (*spotify, -17.26, -6.0, 100);

    REQUIRE (advice.alreadyOnTarget);
    REQUIRE (advice.gainDb == 0.0);
}

TEST_CASE (StreamingTargets_EveryTargetIsPlausibleAndExplained)
{
    REQUIRE_FALSE (streamingTargets().empty());

    for (const auto& target : streamingTargets())
    {
        REQUIRE_FALSE (target.name.empty());
        REQUIRE_FALSE (target.note.empty());

        // Every published streaming target sits in this band. A number outside
        // it is a typo, and a typo here is advice to wreck a recording.
        REQUIRE (target.stereoTargetLufs <= -13.0);
        REQUIRE (target.stereoTargetLufs >= -24.0);

        // Nobody publishes a ceiling at or above full scale.
        REQUIRE (target.truePeakCeilingDbtp <= -1.0);
    }
}
