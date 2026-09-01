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

    /// Which of the two channels the collapsed mono signal should be taken from.
    ///
    /// §2.1's first condition is "one channel stays below -80 dBFS", and it does
    /// not say which -- a device with its capsule wired to the right presents
    /// exactly like one wired to the left. Taking channel 0 regardless is how a
    /// right-wired microphone ends up recording silence, so the side has to be
    /// answered rather than assumed.
    ///
    /// Left unless the left has never risen above the silence floor while the
    /// right has crossed the signal trigger -- the same two thresholds §2.1
    /// already uses. A tie, a duplicated source and ordinary stereo all give
    /// left, so this only ever moves for the case it exists for.
    int getMonoSourceChannel() const noexcept;

    /// Highest peak either channel has reached since the device was seen, in
    /// dBFS. Exposed because "this side has never made a sound" is the evidence
    /// behind getMonoSourceChannel(), and a caller showing its working is worth
    /// more than one asserting a verdict.
    float getLoudestLeftDb() const noexcept { return loudestLeftDb; }
    float getLoudestRightDb() const noexcept { return loudestRightDb; }

private:
    double sampleRate;
    double timeSinceConnection = 0.0;
    double windowElapsed = 0.0;
    bool windowActive = false;
    bool leftSilentWholeWindow = true;
    bool rightSilentWholeWindow = true;

    // Tracked from the first block rather than only inside the measurement
    // window, so the side is answerable immediately. Waiting for the window
    // would mean a right-wired microphone recorded silence for the three
    // seconds §2.1 spends deciding -- and for the full sixty if it stayed
    // quiet.
    float loudestLeftDb = -200.0f;
    float loudestRightDb = -200.0f;
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
