#pragma once
#include <cstdint>

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

    /// Production path: accumulates the raw energy and cross-energy for the
    /// entire signal window, then derives correlation and RMS difference once.
    /// A verdict based on the last callback alone can permanently collapse a
    /// true stereo source just because that one callback happened to match.
    void processBlockEnergies (float leftPeakDb, float rightPeakDb,
                               double sumLeftSquared, double sumRightSquared,
                               double sumLeftRight, int sampleCount,
                               double blockSeconds) noexcept;

    ChannelLayoutDecision getDecision() const noexcept { return decision; }

    /// True only for a verdict backed by the three-second signal window. The
    /// sixty-second no-signal Mono answer is deliberately provisional: it is
    /// useful as a safe local fallback, but must not be persisted and hide the
    /// second socket of a quiet interface forever.
    bool isDecisionPersistable() const noexcept { return decisionPersistable; }

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
    /// Left until one callback carries clear one-sided evidence. Silence keeps
    /// the last evidenced side, while a later live opposite side may correct
    /// it before the verdict freezes. A tie, duplicated source and ordinary
    /// stereo do not move it.
    int getMonoSourceChannel() const noexcept;

    /// Whether a callback has carried one side above the signal trigger while
    /// the other stayed below the silence floor. Silence on both sides is not
    /// evidence for changing a remembered physical source.
    bool hasMonoSourceEvidence() const noexcept;

    /// Highest peak either channel has reached since the device was seen, in
    /// dBFS. Useful diagnostic evidence alongside the verdict.
    float getLoudestLeftDb() const noexcept { return loudestLeftDb; }
    float getLoudestRightDb() const noexcept { return loudestRightDb; }

private:
    double sampleRate;
    double timeSinceConnection = 0.0;
    double windowElapsed = 0.0;
    bool windowActive = false;
    bool leftSilentWholeWindow = true;
    bool rightSilentWholeWindow = true;
    double windowSumLeftSquared = 0.0;
    double windowSumRightSquared = 0.0;
    double windowSumLeftRight = 0.0;
    uint64_t windowSampleCount = 0;

    // Tracked from the first block rather than only inside the measurement
    // window, so the side is answerable immediately. Waiting for the window
    // would mean a right-wired microphone recorded silence for the three
    // seconds §2.1 spends deciding -- and for the full sixty if it stayed
    // quiet.
    float loudestLeftDb = -200.0f;
    float loudestRightDb = -200.0f;
    int monoSourceEvidence = -1;
    ChannelLayoutDecision decision = ChannelLayoutDecision::Pending;
    bool decisionPersistable = false;

    static constexpr float kSignalTriggerDb = -50.0f;
    static constexpr float kSilenceThresholdDb = -80.0f;
    static constexpr float kCorrelationThreshold = 0.99f;
    static constexpr float kRmsDiffThresholdDb = 0.5f;
    static constexpr double kWindowSeconds = 3.0;
    static constexpr double kTimeoutSeconds = 60.0;

    void finalizeWindow() noexcept;
};

} // namespace mma
