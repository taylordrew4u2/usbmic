#include "ChannelLayoutAnalyzer.h"

namespace mma {

ChannelLayoutAnalyzer::ChannelLayoutAnalyzer (double sampleRateIn) noexcept
    : sampleRate (sampleRateIn)
{
}

void ChannelLayoutAnalyzer::processBlock (float leftPeakDb, float rightPeakDb,
                                          float correlation, float rmsDiffDb,
                                          double blockSeconds) noexcept
{
    if (decision != ChannelLayoutDecision::Pending)
        return;

    timeSinceConnection += blockSeconds;

    if (! windowActive)
    {
        if (leftPeakDb > kSignalTriggerDb || rightPeakDb > kSignalTriggerDb)
        {
            windowActive = true;
            windowElapsed = 0.0;
            leftSilentWholeWindow = true;
            rightSilentWholeWindow = true;
            // Fall through: this same block already counts as the first sample
            // of the measurement window rather than being discarded.
        }
        else
        {
            if (timeSinceConnection >= kTimeoutSeconds)
            {
                // Never block on this; default to mono and let signal re-evaluation happen later.
                decision = ChannelLayoutDecision::Mono;
            }
            return;
        }
    }

    // Inside the 3s measurement window.
    if (! (leftPeakDb < kSilenceThresholdDb))
        leftSilentWholeWindow = false;
    if (! (rightPeakDb < kSilenceThresholdDb))
        rightSilentWholeWindow = false;

    windowElapsed += blockSeconds;
    if (windowElapsed >= kWindowSeconds - 1.0e-9)
        finalizeWindow (leftPeakDb, rightPeakDb, correlation, rmsDiffDb);
}

void ChannelLayoutAnalyzer::finalizeWindow (float leftPeakDb, float rightPeakDb,
                                            float correlation, float rmsDiffDb) noexcept
{
    (void) leftPeakDb;
    (void) rightPeakDb;

    const bool oneSideSilentWholeWindow = leftSilentWholeWindow || rightSilentWholeWindow;
    const bool correlatedAndMatched = (correlation > kCorrelationThreshold) && (rmsDiffDb < kRmsDiffThresholdDb);

    decision = (oneSideSilentWholeWindow || correlatedAndMatched)
                   ? ChannelLayoutDecision::Mono
                   : ChannelLayoutDecision::Stereo;
}

} // namespace mma
