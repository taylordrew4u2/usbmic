#pragma once
#include <vector>

namespace mma {

/// §8.1: a channel is "dead" (likely hardware-muted or unplugged) if it stays
/// below -60dBFS for 20 continuous seconds while at least one other channel
/// exceeds -40dBFS at some point in that same window.
class DeadChannelDetector
{
public:
    static constexpr float kDeadThresholdDb = -60.0f;
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
