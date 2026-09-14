#pragma once
#include "Metering.h"
#include <vector>

namespace mma {

/// §8.1: a channel is "dead" (likely hardware-muted or unplugged) if it stays
/// below -60dBFS for 20 continuous seconds while at least one other channel
/// exceeds -40dBFS at some point in that same window.
class DeadChannelDetector
{
public:
    static constexpr float kDeadThresholdDb = -60.0f;

    // The threshold has to be REACHABLE by the levels this detector is fed.
    // Metering floors at kMinDb, so a threshold below that floor can never be
    // crossed and the detector becomes dead code that still passes its tests.
    // If either constant moves, this stops the build rather than silently
    // turning the silent-channel warning off again.
    static_assert (kDeadThresholdDb >= Metering::kMinDb,
                   "a dead-channel threshold below the metering floor can never be reached");
    static constexpr float kOtherActiveThresholdDb = -40.0f;
    static constexpr double kSustainSeconds = 20.0;

    explicit DeadChannelDetector (int numChannels);

    void reset();

    /// peaksDb.size() must equal numChannels. Call once per audio block.
    void processBlock (const std::vector<float>& peaksDb, double blockSeconds);

    bool isChannelDead (int channelIndex) const;

private:
    int numChannels;
    std::vector<double> belowThresholdSeconds;
    std::vector<bool> anotherChannelWasActiveDuringWindow;
    std::vector<bool> dead;
};

} // namespace mma
