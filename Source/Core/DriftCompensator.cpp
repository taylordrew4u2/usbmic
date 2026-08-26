#include "DriftCompensator.h"
#include <algorithm>
#include <cmath>

namespace mma {

DriftCompensator::DriftCompensator (double sampleRateIn) noexcept
    : sampleRate (sampleRateIn)
{
}

void DriftCompensator::reset() noexcept
{
    integralTerm = 0.0;
    currentPpm = 0.0;
    excessDriftSeconds = 0.0;
    sustainedExcessDrift = false;
}

void DriftCompensator::update (double fillError, int blockSizeSamples) noexcept
{
    integralTerm += kKi * fillError;

    const double proportional = kKp * fillError;
    double target = proportional + integralTerm;

    // Clamp to the +/-200ppm ratio deviation ceiling (in fractional-ratio units).
    const double maxDeviation = kMaxRatioDeviationPpm * 1.0e-6;
    target = std::clamp (target, -maxDeviation, maxDeviation);

    // Never correct instantaneously: limit slew to 5 PPM/second, expressed per this block.
    const double blockSeconds = (sampleRate > 0.0) ? (static_cast<double> (blockSizeSamples) / sampleRate) : 0.0;
    const double maxStep = (kMaxSlewPpmPerSecond * 1.0e-6) * blockSeconds;

    double currentRatioDeviation = currentPpm * 1.0e-6;
    const double delta = std::clamp (target - currentRatioDeviation, -maxStep, maxStep);
    currentRatioDeviation += delta;
    currentRatioDeviation = std::clamp (currentRatioDeviation, -maxDeviation, maxDeviation);

    currentPpm = currentRatioDeviation * 1.0e6;

    // Keep the integral term consistent with the clamped output so it doesn't wind up unboundedly.
    integralTerm = std::clamp (integralTerm, -maxDeviation, maxDeviation);
}

void DriftCompensator::updateSustainedDriftFlag (double elapsedSeconds) noexcept
{
    if (std::abs (currentPpm) > kExcessDriftThresholdPpm)
    {
        excessDriftSeconds += elapsedSeconds;
        if (excessDriftSeconds >= kExcessDriftSustainSeconds)
            sustainedExcessDrift = true;
    }
    else
    {
        excessDriftSeconds = 0.0;
        sustainedExcessDrift = false;
    }
}

} // namespace mma
