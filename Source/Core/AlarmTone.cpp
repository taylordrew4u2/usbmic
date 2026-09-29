#include "AlarmTone.h"
#include <chrono>
#include <cmath>

namespace mma {

namespace {
constexpr double kTwoPi = 6.283185307179586;
constexpr double kChirpSeconds = AlarmTone::kBeepsPerChirp * (AlarmTone::kBeepSeconds + AlarmTone::kGapSeconds);
} // namespace

int64_t AlarmTone::nowNs() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds> (
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void AlarmTone::trigger (Kind newKind) noexcept
{
    triggeredAtNs.store (nowNs(), std::memory_order_relaxed);
    patternFinished.store (false, std::memory_order_relaxed);
    kind.store (static_cast<int> (newKind), std::memory_order_release);
    generation.fetch_add (1, std::memory_order_acq_rel);
}

void AlarmTone::setFault (bool on) noexcept
{
    const auto current = getKind();

    if (on && current != Kind::Fault)
        trigger (Kind::Fault);
    else if (! on && current == Kind::Fault)
        trigger (Kind::None);
}

bool AlarmTone::isSounding() const noexcept
{
    switch (getKind())
    {
        case Kind::None:    return false;
        case Kind::Fault:   return true;
        case Kind::Started:
        case Kind::Stopped:
        {
            if (patternFinished.load (std::memory_order_relaxed))
                return false;

            const auto elapsed = static_cast<double> (nowNs() - triggeredAtNs.load (std::memory_order_relaxed)) * 1.0e-9;
            return elapsed < kChirpSeconds;
        }
    }

    return false;
}

float AlarmTone::frequencyAt (Kind kind, double seconds, bool& sounding) noexcept
{
    sounding = false;

    switch (kind)
    {
        case Kind::None:
            return 0.0f;

        case Kind::Started:
        case Kind::Stopped:
        {
            if (seconds >= kChirpSeconds)
                return 0.0f;

            const double slot = kBeepSeconds + kGapSeconds;
            const int beep = static_cast<int> (seconds / slot);
            const double within = seconds - beep * slot;

            if (within >= kBeepSeconds)
                return 0.0f; // the gap

            sounding = true;

            // A major triad, up for a start and down for a stop: two sounds
            // nobody confuses even from across the room.
            static constexpr float rising[kBeepsPerChirp] = { 660.0f, 880.0f, 1320.0f };
            const int index = kind == Kind::Started ? beep : (kBeepsPerChirp - 1 - beep);
            return rising[index < 0 ? 0 : (index >= kBeepsPerChirp ? kBeepsPerChirp - 1 : index)];
        }

        case Kind::Fault:
        {
            sounding = true;
            const auto half = static_cast<int64_t> (seconds / kSirenHalfPeriodSeconds);
            return (half % 2 == 0) ? 880.0f : 1320.0f;
        }
    }

    return 0.0f;
}

void AlarmTone::render (float* inOut, int numSamples, double sampleRate) noexcept
{
    const auto currentKind = getKind();
    const auto currentGeneration = generation.load (std::memory_order_acquire);

    if (currentGeneration != renderedGeneration)
    {
        renderedGeneration = currentGeneration;
        positionSeconds = 0.0;
        phase = 0.0;
    }

    if (currentKind == Kind::None || inOut == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return;

    const float level = currentKind == Kind::Fault ? kSirenLevel : kChirpLevel;
    const double step = 1.0 / sampleRate;
    uint64_t rendered = 0;

    for (int i = 0; i < numSamples; ++i)
    {
        bool sounding = false;
        const float frequency = frequencyAt (currentKind, positionSeconds, sounding);

        if (sounding)
        {
            // A short fade at each edge keeps the beeps from clicking.
            const double slot = kBeepSeconds + kGapSeconds;
            const double within = currentKind == Kind::Fault
                ? std::fmod (positionSeconds, kSirenHalfPeriodSeconds)
                : std::fmod (positionSeconds, slot);
            const double edge = 0.004;
            const double window = currentKind == Kind::Fault ? kSirenHalfPeriodSeconds : kBeepSeconds;
            double envelope = 1.0;
            if (within < edge)
                envelope = within / edge;
            else if (within > window - edge)
                envelope = (window - within) / edge;
            if (envelope < 0.0) envelope = 0.0;

            inOut[i] += level * static_cast<float> (envelope * std::sin (phase));
            phase += kTwoPi * frequency * step;
            if (phase > kTwoPi)
                phase -= kTwoPi;
            ++rendered;
        }

        positionSeconds += step;
    }

    if (currentKind != Kind::Fault && positionSeconds >= kChirpSeconds)
        patternFinished.store (true, std::memory_order_relaxed);

    if (rendered > 0)
        samplesRendered.fetch_add (rendered, std::memory_order_relaxed);
}

} // namespace mma
