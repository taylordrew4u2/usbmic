#include "TestFramework.h"
#include "Core/AlarmTone.h"
#include <cmath>
#include <vector>

using namespace mma;

namespace {

constexpr double kRate = 48000.0;

/// Renders `seconds` of tone in 256-sample blocks and returns the buffer.
std::vector<float> renderSeconds (AlarmTone& tone, double seconds)
{
    std::vector<float> out (static_cast<size_t> (seconds * kRate), 0.0f);
    for (size_t at = 0; at < out.size(); at += 256)
        tone.render (out.data() + at, static_cast<int> (std::min<size_t> (256, out.size() - at)), kRate);
    return out;
}

float peakBetween (const std::vector<float>& v, double fromSeconds, double toSeconds)
{
    float peak = 0.0f;
    const auto from = static_cast<size_t> (fromSeconds * kRate);
    const auto to = std::min (v.size(), static_cast<size_t> (toSeconds * kRate));
    for (size_t i = from; i < to; ++i)
        peak = std::max (peak, std::abs (v[i]));
    return peak;
}

/// Zero crossings per second over a window: a cheap frequency estimate.
double frequencyBetween (const std::vector<float>& v, double fromSeconds, double toSeconds)
{
    const auto from = static_cast<size_t> (fromSeconds * kRate);
    const auto to = std::min (v.size(), static_cast<size_t> (toSeconds * kRate));
    int crossings = 0;
    for (size_t i = from + 1; i < to; ++i)
        if ((v[i - 1] < 0.0f) != (v[i] < 0.0f))
            ++crossings;
    return crossings / 2.0 / (toSeconds - fromSeconds);
}

} // namespace

TEST_CASE (AlarmTone_SilentUntilTriggered)
{
    AlarmTone tone;
    REQUIRE_FALSE (tone.isSounding());

    const auto out = renderSeconds (tone, 0.5);
    REQUIRE (peakBetween (out, 0.0, 0.5) == 0.0f);
    REQUIRE (tone.getSamplesRendered() == 0);
}

TEST_CASE (AlarmTone_StartChirpIsThreeRisingBeepsThenSilence)
{
    AlarmTone tone;
    tone.trigger (AlarmTone::Kind::Started);
    REQUIRE (tone.isSounding());

    const auto out = renderSeconds (tone, 1.0);
    const double slot = AlarmTone::kBeepSeconds + AlarmTone::kGapSeconds;

    // Each beep sounds, each gap is silent, and the pitch climbs.
    double lastFrequency = 0.0;
    for (int beep = 0; beep < AlarmTone::kBeepsPerChirp; ++beep)
    {
        const double start = beep * slot + 0.01;
        const double end = beep * slot + AlarmTone::kBeepSeconds - 0.01;
        REQUIRE (peakBetween (out, start, end) > 0.2f);
        REQUIRE (peakBetween (out, start, end) <= AlarmTone::kChirpLevel + 0.01f);

        const double frequency = frequencyBetween (out, start, end);
        REQUIRE (frequency > lastFrequency * 1.2);
        lastFrequency = frequency;

        const double gapStart = beep * slot + AlarmTone::kBeepSeconds + 0.005;
        REQUIRE (peakBetween (out, gapStart, (beep + 1) * slot - 0.001) == 0.0f);
    }

    // Once, not forever.
    REQUIRE (peakBetween (out, AlarmTone::kBeepsPerChirp * slot, 1.0) == 0.0f);
    REQUIRE_FALSE (tone.isSounding());
}

TEST_CASE (AlarmTone_StopChirpFalls)
{
    AlarmTone tone;
    tone.trigger (AlarmTone::Kind::Stopped);
    const auto out = renderSeconds (tone, 1.0);
    const double slot = AlarmTone::kBeepSeconds + AlarmTone::kGapSeconds;

    const double first = frequencyBetween (out, 0.01, AlarmTone::kBeepSeconds - 0.01);
    const double last = frequencyBetween (out, 2 * slot + 0.01, 2 * slot + AlarmTone::kBeepSeconds - 0.01);
    REQUIRE (first > last * 1.5);
}

TEST_CASE (AlarmTone_FaultSirenAlternatesAndRepeatsUntilCleared)
{
    AlarmTone tone;
    tone.setFault (true);
    REQUIRE (tone.isSounding());
    REQUIRE (tone.getKind() == AlarmTone::Kind::Fault);

    const auto out = renderSeconds (tone, 3.0);
    const double half = AlarmTone::kSirenHalfPeriodSeconds;

    // Still going after two seconds, louder than a chirp, never past the level.
    REQUIRE (peakBetween (out, 2.5, 3.0) > 0.4f);
    REQUIRE (peakBetween (out, 0.0, 3.0) <= AlarmTone::kSirenLevel + 0.01f);

    const double low = frequencyBetween (out, 0.02, half - 0.02);
    const double high = frequencyBetween (out, half + 0.02, 2 * half - 0.02);
    REQUIRE (high > low * 1.3);

    // Re-asserting it is a no-op: no phase restart, no click.
    tone.setFault (true);
    REQUIRE (tone.getKind() == AlarmTone::Kind::Fault);

    tone.setFault (false);
    REQUIRE_FALSE (tone.isSounding());
    const auto after = renderSeconds (tone, 0.2);
    REQUIRE (peakBetween (after, 0.0, 0.2) == 0.0f);
}

TEST_CASE (AlarmTone_AddsToTheMixRatherThanReplacingIt)
{
    AlarmTone tone;
    tone.setFault (true);

    std::vector<float> mix (256, 0.25f);
    tone.render (mix.data(), 256, kRate);

    // Every sample still carries the 0.25 the mix had, plus the tone.
    bool anyDifferent = false;
    for (auto s : mix)
    {
        REQUIRE (s >= 0.25f - AlarmTone::kSirenLevel - 0.01f);
        anyDifferent = anyDifferent || std::abs (s - 0.25f) > 0.01f;
    }
    REQUIRE (anyDifferent);
    REQUIRE (tone.getSamplesRendered() > 0);
}

TEST_CASE (AlarmTone_ClearingFromAChirpDoesNotSound)
{
    AlarmTone tone;
    tone.trigger (AlarmTone::Kind::Started);
    tone.setFault (false); // a fault is not on; the chirp must be left alone
    REQUIRE (tone.getKind() == AlarmTone::Kind::Started);

    tone.trigger (AlarmTone::Kind::None);
    REQUIRE_FALSE (tone.isSounding());
    const auto out = renderSeconds (tone, 0.2);
    REQUIRE (peakBetween (out, 0.0, 0.2) == 0.0f);
}
