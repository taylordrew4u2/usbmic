#include "ChannelLayoutAnalyzer.h"
#include <algorithm>
#include <cmath>

namespace mma {

ChannelLayoutAnalyzer::ChannelLayoutAnalyzer (double sampleRateIn) noexcept
    : sampleRate (sampleRateIn)
{
}

int ChannelLayoutAnalyzer::getMonoSourceChannel() const noexcept
{
    return monoSourceEvidence == 1 ? 1 : 0;
}

bool ChannelLayoutAnalyzer::hasMonoSourceEvidence() const noexcept
{
    return monoSourceEvidence >= 0;
}

void ChannelLayoutAnalyzer::processBlock (float leftPeakDb, float rightPeakDb,
                                          float correlation, float rmsDiffDb,
                                          double blockSeconds) noexcept
{
    // Compatibility path for the small pure tests and any caller that has
    // already reduced a block to correlation/RMS difference. Reconstruct a
    // normalized energy block so it participates in the same whole-window
    // accumulation as the production callback.
    const int sampleCount = std::max (1, static_cast<int> (std::llround (
        std::max (0.0, blockSeconds) * std::max (1.0, sampleRate))));
    const double rightRms = std::pow (10.0, -std::max (0.0f, rmsDiffDb) / 20.0);
    const double sumLeftSquared = static_cast<double> (sampleCount);
    const double sumRightSquared = static_cast<double> (sampleCount) * rightRms * rightRms;
    const double boundedCorrelation = std::clamp (static_cast<double> (correlation), -1.0, 1.0);
    const double sumLeftRight = boundedCorrelation
                              * std::sqrt (sumLeftSquared * sumRightSquared);

    processBlockEnergies (leftPeakDb, rightPeakDb,
                          sumLeftSquared, sumRightSquared, sumLeftRight,
                          sampleCount, blockSeconds);
}

void ChannelLayoutAnalyzer::processBlockEnergies (float leftPeakDb, float rightPeakDb,
                                                   double sumLeftSquared,
                                                   double sumRightSquared,
                                                   double sumLeftRight,
                                                   int sampleCount,
                                                   double blockSeconds) noexcept
{
    // Peaks are tracked even after the final verdict lands: the decision stops
    // moving, but "how loud has this side ever been" stays true and costs two
    // compares. The provisional silent timeout is not final and continues.
    loudestLeftDb = (leftPeakDb > loudestLeftDb) ? leftPeakDb : loudestLeftDb;
    loudestRightDb = (rightPeakDb > loudestRightDb) ? rightPeakDb : loudestRightDb;

    if (decisionPersistable)
        return;

    // Current one-sided evidence can correct a remembered source after any
    // number of quiet callbacks. Keeping the latest clear observation avoids
    // one transient on the old side poisoning the choice for the connection's
    // entire lifetime; ambiguous and silent blocks leave it untouched.
    if (leftPeakDb > kSignalTriggerDb && rightPeakDb < kSilenceThresholdDb)
        monoSourceEvidence = 0;
    else if (rightPeakDb > kSignalTriggerDb && leftPeakDb < kSilenceThresholdDb)
        monoSourceEvidence = 1;

    timeSinceConnection += blockSeconds;

    if (! windowActive)
    {
        if (leftPeakDb > kSignalTriggerDb || rightPeakDb > kSignalTriggerDb)
        {
            // A no-signal timeout is a provisional Mono fallback, not a final
            // classification. The first real signal reopens the measurement
            // window so a quiet two-input interface can still prove Stereo.
            decision = ChannelLayoutDecision::Pending;
            windowActive = true;
            windowElapsed = 0.0;
            leftSilentWholeWindow = true;
            rightSilentWholeWindow = true;
            windowSumLeftSquared = 0.0;
            windowSumRightSquared = 0.0;
            windowSumLeftRight = 0.0;
            windowSampleCount = 0;
            // Fall through: this same block already counts as the first sample
            // of the measurement window rather than being discarded.
        }
        else
        {
            if (timeSinceConnection >= kTimeoutSeconds)
            {
                // Never block on this; default provisionally to mono and let
                // later signal re-evaluate it. Callers must not persist this.
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

    // Accumulate before testing the duration so the callback that crosses the
    // three-second boundary is part of the answer -- but only as one block in
    // the full window, never as a replacement for it. Non-finite input cannot
    // be trusted as evidence for correlation, so it contributes silence.
    if (sampleCount > 0)
    {
        windowSumLeftSquared += std::isfinite (sumLeftSquared)
                                  && sumLeftSquared > 0.0 ? sumLeftSquared : 0.0;
        windowSumRightSquared += std::isfinite (sumRightSquared)
                                   && sumRightSquared > 0.0 ? sumRightSquared : 0.0;
        windowSumLeftRight += std::isfinite (sumLeftRight) ? sumLeftRight : 0.0;
        windowSampleCount += static_cast<uint64_t> (sampleCount);
    }

    windowElapsed += blockSeconds;
    if (windowElapsed >= kWindowSeconds - 1.0e-9)
        finalizeWindow();
}

void ChannelLayoutAnalyzer::finalizeWindow() noexcept
{
    const double denominator = std::sqrt (windowSumLeftSquared * windowSumRightSquared);
    const double correlation = denominator > 1.0e-12
                                 ? windowSumLeftRight / denominator : 0.0;

    const double samples = static_cast<double> (std::max<uint64_t> (1, windowSampleCount));
    const double rmsLeft = std::sqrt (windowSumLeftSquared / samples);
    const double rmsRight = std::sqrt (windowSumRightSquared / samples);
    const auto toDb = [] (double linear)
    {
        return linear > 1.0e-10 ? 20.0 * std::log10 (linear) : -200.0;
    };
    const double rmsDiffDb = std::abs (toDb (rmsLeft) - toDb (rmsRight));

    const bool oneSideSilentWholeWindow = leftSilentWholeWindow || rightSilentWholeWindow;
    const bool correlatedAndMatched = (correlation > kCorrelationThreshold) && (rmsDiffDb < kRmsDiffThresholdDb);

    decision = (oneSideSilentWholeWindow || correlatedAndMatched)
                   ? ChannelLayoutDecision::Mono
                   : ChannelLayoutDecision::Stereo;
    decisionPersistable = true;
}

} // namespace mma
