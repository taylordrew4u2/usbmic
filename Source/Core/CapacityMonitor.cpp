#include "CapacityMonitor.h"

namespace mma {

WritePipelineState CapacityMonitor::evaluateFill (double fillFraction) noexcept
{
    // Card and mirror writes are serialized after this same ring. A mirror may
    // protect against the card disappearing, but it cannot protect either copy
    // from samples the shared ring has already dropped. Shed both sets of stems
    // before that happens and keep the complete mix(es) moving.
    if (fillFraction >= kFillDegradeFraction)
        return WritePipelineState::DegradedToMixOnly;

    if (fillFraction >= kFillWarningFraction)
        return WritePipelineState::FillWarning;

    return WritePipelineState::Healthy;
}

RemainingTimeWarning CapacityMonitor::evaluateRemaining (double remainingSeconds) noexcept
{
    if (remainingSeconds <= 0.0)
    {
        if (warnedExhausted)
            return RemainingTimeWarning::None;

        warnedExhausted = true;
        // Everything below is moot once the card is full; latch them so a later
        // call cannot emit a stale ten-minute warning.
        warnedTenMinutes = true;
        warnedTwoMinutes = true;
        return RemainingTimeWarning::Exhausted;
    }

    if (remainingSeconds <= kTwoMinutesSeconds && ! warnedTwoMinutes)
    {
        warnedTwoMinutes = true;
        warnedTenMinutes = true; // crossing straight past ten shouldn't re-warn later
        return RemainingTimeWarning::TwoMinutes;
    }

    if (remainingSeconds <= kTenMinutesSeconds && ! warnedTenMinutes)
    {
        warnedTenMinutes = true;
        return RemainingTimeWarning::TenMinutes;
    }

    return RemainingTimeWarning::None;
}

void CapacityMonitor::noteDegradationAt (long long samplePosition) noexcept
{
    if (degradationSamplePosition < 0)
        degradationSamplePosition = samplePosition;
}

void CapacityMonitor::reset() noexcept
{
    warnedTenMinutes = false;
    warnedTwoMinutes = false;
    warnedExhausted = false;
    degradationSamplePosition = -1;
}

} // namespace mma
