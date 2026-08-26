#include "Metering.h"
#include <algorithm>
#include <cmath>

namespace mma {

Metering::Metering (double sampleRateIn) noexcept
    : sampleRate (sampleRateIn)
{
}

float Metering::linearToDb (float linear) noexcept
{
    if (linear <= 0.0f)
        return kMinDb;
    return std::max (kMinDb, 20.0f * std::log10 (linear));
}

void Metering::processAudioBlock (const float* samples, int numSamples) noexcept
{
    float maxAbs = 0.0f;
    int consecutive = consecutiveClipSamples.load (std::memory_order_relaxed);
    const float clipLinear = std::pow (10.0f, kClipThresholdDb / 20.0f);

    for (int i = 0; i < numSamples; ++i)
    {
        const float a = std::abs (samples[i]);
        maxAbs = std::max (maxAbs, a);

        if (a >= clipLinear)
        {
            ++consecutive;
            if (consecutive >= kClipConsecutiveSamples)
            {
                clipLatched.store (true, std::memory_order_relaxed);
                clipCount.fetch_add (1, std::memory_order_relaxed);
                consecutive = 0; // start counting the next clip event independently
            }
        }
        else
        {
            consecutive = 0;
        }
    }

    consecutiveClipSamples.store (consecutive, std::memory_order_relaxed);
    latestBlockPeakDb.store (linearToDb (maxAbs), std::memory_order_relaxed);
}

void Metering::pushBlockStats (float maxAbsLinear, int /*numSamplesInBlock*/) noexcept
{
    latestBlockPeakDb.store (linearToDb (maxAbsLinear), std::memory_order_relaxed);
    if (maxAbsLinear >= std::pow (10.0f, kClipThresholdDb / 20.0f))
    {
        clipLatched.store (true, std::memory_order_relaxed);
        clipCount.fetch_add (1, std::memory_order_relaxed);
    }
}

float Metering::tick (double dtSeconds) noexcept
{
    const float blockDb = latestBlockPeakDb.load (std::memory_order_relaxed);

    // Exponential attack/decay envelope toward the incoming block level.
    const bool rising = blockDb > displayedDb;
    const double timeConstant = rising ? kAttackSeconds : kDecaySeconds;
    const double coeff = (timeConstant > 0.0) ? std::exp (-dtSeconds / timeConstant) : 0.0;
    displayedDb = static_cast<float> (blockDb + (displayedDb - blockDb) * coeff);
    displayedDb = std::clamp (displayedDb, kMinDb, kMaxDb);

    // Peak hold: latch a new peak immediately, hold for kPeakHoldSeconds, then
    // decay at kPeakDecayDbPerSecond.
    if (blockDb >= peakHoldDb)
    {
        peakHoldDb = blockDb;
        peakHoldElapsed = 0.0;
    }
    else
    {
        peakHoldElapsed += dtSeconds;
        if (peakHoldElapsed > kPeakHoldSeconds)
        {
            const double decaying = peakHoldElapsed - kPeakHoldSeconds;
            peakHoldDb = static_cast<float> (peakHoldDb - kPeakDecayDbPerSecond * dtSeconds);
            peakHoldDb = std::max (peakHoldDb, blockDb);
            peakHoldDb = std::clamp (peakHoldDb, kMinDb, kMaxDb);
            (void) decaying;
        }
    }

    return displayedDb;
}

void Metering::acknowledgeClip() noexcept
{
    clipLatched.store (false, std::memory_order_relaxed);
    consecutiveClipSamples.store (0, std::memory_order_relaxed);
}

} // namespace mma
