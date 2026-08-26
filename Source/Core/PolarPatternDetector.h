#pragma once
#include <cstddef>

namespace mma {

/// §14.4: detects a likely non-cardioid microphone from inter-channel correlation.
/// Correlation > 0.6 sustained for 10s between a pair of channels, while a third
/// channel stays below -45dBFS, indicates room bleed from an omni/stereo pattern.
class PolarPatternDetector
{
public:
    static constexpr float kCorrelationThreshold = 0.6f;
    static constexpr float kThirdChannelSilenceDb = -45.0f;
    static constexpr double kSustainSeconds = 10.0;

    void reset() noexcept;

    /// channelACorrelationWithB: correlation between the two suspect channels this block.
    /// thirdChannelPeakDb: peak level of a third, uninvolved channel in the same block.
    /// Returns true once the sustained condition has been met (latches until reset()).
    bool processBlock (float channelACorrelationWithB, float thirdChannelPeakDb, double blockSeconds) noexcept;

    bool isTriggered() const noexcept { return triggered; }

private:
    double sustainedSeconds = 0.0;
    bool triggered = false;
};

} // namespace mma
