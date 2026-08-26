#include "PolarPatternDetector.h"

namespace mma {

void PolarPatternDetector::reset() noexcept
{
    sustainedSeconds = 0.0;
    triggered = false;
}

bool PolarPatternDetector::processBlock (float channelACorrelationWithB, float thirdChannelPeakDb, double blockSeconds) noexcept
{
    const bool conditionMet = (channelACorrelationWithB > kCorrelationThreshold) && (thirdChannelPeakDb < kThirdChannelSilenceDb);

    if (conditionMet)
    {
        sustainedSeconds += blockSeconds;
        if (sustainedSeconds >= kSustainSeconds)
            triggered = true;
    }
    else
    {
        sustainedSeconds = 0.0;
    }

    return triggered;
}

} // namespace mma
