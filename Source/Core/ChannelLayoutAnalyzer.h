#pragma once

namespace mma {

enum class ChannelLayoutDecision
{
    Pending,   // still gathering the 3s signal window
    Mono,      // collapsed to mono
    Stereo     // kept as true stereo
};

/// Mono-collapse detection per spec §2.1. Feed it block-level stats for a
/// stereo device's left/right channels; it decides whether the device should
/// be treated as mono (one silent side, or a duplicated source) or true stereo.
class ChannelLayoutAnalyzer
{
public:
    explicit ChannelLayoutAnalyzer (double sampleRate) noexcept;

    /// Call once per audio block with per-channel peak (dBFS) and the raw samples'
    /// running correlation/RMS-difference inputs. secondsSinceConnection tracks total
    /// elapsed time since the device was first seen, for the 60s timeout rule.
    void processBlock (float leftPeakDb, float rightPeakDb,
                       float correlation, float rmsDiffDb,
                       double blockSeconds) noexcept;

    ChannelLayoutDecision getDecision() const noexcept { return decision; }

    /// True once either channel has crossed -50dBFS, starting the 3s measurement window.
    bool isWindowActive() const noexcept { return windowActive; }

private:
    double sampleRate;
    double timeSinceConnection = 0.0;
    double windowElapsed = 0.0;
    bool windowActive = false;
    bool leftSilentWholeWindow = true;
    bool rightSilentWholeWindow = true;
    ChannelLayoutDecision decision = ChannelLayoutDecision::Pending;

    static constexpr float kSignalTriggerDb = -50.0f;
    static constexpr float kSilenceThresholdDb = -80.0f;
    static constexpr float kCorrelationThreshold = 0.99f;
    static constexpr float kRmsDiffThresholdDb = 0.5f;
    static constexpr double kWindowSeconds = 3.0;
    static constexpr double kTimeoutSeconds = 60.0;

    void finalizeWindow (float leftPeakDb, float rightPeakDb,
                         float correlation, float rmsDiffDb) noexcept;
};

} // namespace mma
