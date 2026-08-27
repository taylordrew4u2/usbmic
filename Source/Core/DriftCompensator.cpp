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
    const double maxDeviation = kMaxRatioDeviationPpm * 1.0e-6;

    const double proportional = kKp * fillError;
    const double candidateIntegral = std::clamp (integralTerm + kKi * fillError,
                                                 -maxDeviation, maxDeviation);

    const double target = std::clamp (proportional + candidateIntegral, -maxDeviation, maxDeviation);

    // Never correct instantaneously: limit slew to 5 PPM/second, expressed per this block.
    const double blockSeconds = (sampleRate > 0.0) ? (static_cast<double> (blockSizeSamples) / sampleRate) : 0.0;
    const double maxStep = (kMaxSlewPpmPerSecond * 1.0e-6) * blockSeconds;

    double currentRatioDeviation = currentPpm * 1.0e-6;
    const double wanted = target - currentRatioDeviation;
    const double delta = std::clamp (wanted, -maxStep, maxStep);

    // Anti-windup. The actuator is deliberately rate limited, so while it is
    // still slewing towards target the loop cannot deliver what the integral is
    // asking for. Integrating anyway stores up correction that has to be
    // unwound later -- at the same 5 PPM/s -- and the result is a loop that
    // swings past target every time instead of settling on it. Integration
    // therefore pauses whenever the output is rate- or clamp-limited, which is
    // what makes this a PI controller rather than a PI controller's arithmetic.
    if (delta == wanted)
        integralTerm = candidateIntegral;

    currentRatioDeviation = std::clamp (currentRatioDeviation + delta, -maxDeviation, maxDeviation);
    currentPpm = currentRatioDeviation * 1.0e6;
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
