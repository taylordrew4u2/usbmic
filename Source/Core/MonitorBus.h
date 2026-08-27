#pragma once
#include <vector>
#include <cstddef>

namespace mma {

/// §5: the live monitor mix. Unity sum (no per-channel attenuation with channel
/// count -- level is managed by trim and master volume only), a mandatory
/// zero-lookahead brickwall limiter at -3dBFS, a runaway cut if that limiter
/// stays engaged for >500ms, third-octave feedback detection, and a global mute.
/// This bus's limiter is a *separate instance* from MixBusLimiter (§6.1) so a
/// monitor mute or runaway cut can never silence the recorded mix file.
class MonitorBus
{
public:
    static constexpr float kLimiterCeilingDb = -3.0f;
    static constexpr double kLimiterReleaseSeconds = 0.001; // 1ms release
    static constexpr double kRunawayCutSeconds = 0.5;       // 500ms continuous engagement -> mute

    static constexpr double kFeedbackBandRiseDb = 10.0;
    static constexpr double kFeedbackWindowSeconds = 0.5;
    static constexpr double kFeedbackWithinBroadbandDb = 6.0;

    static constexpr double kDefaultMonitorVolume = 70.0; // 0-100
    static constexpr double kMinTrimDb = -20.0;
    static constexpr double kMaxTrimDb = 20.0;
    static constexpr double kTrimStepDb = 0.5;

    explicit MonitorBus (double sampleRate) noexcept;

    /// Sum trimmed input channels to a single mono/stereo monitor sample, apply the
    /// safety limiter, and return the limited sample. No EQ, no filtering, no
    /// lookahead -- only summing, trim (applied by caller before summing), and this
    /// limiter belong on this bus.
    float processSample (const std::vector<float>& trimmedInputSamples) noexcept;

    /// Maps 0-100 UI volume to a linear gain, logarithmically, per §5.1.
    static float monitorVolumeToLinearGain (double volume0to100) noexcept;

    /// §5.1 master monitor volume, 0-100, default 70. Affects only what reaches
    /// the headphones; recorded files are written from a separate path.
    void setMasterVolume (double volume0to100) noexcept;
    double getMasterVolume() const noexcept { return masterVolume; }

    /// Output-stage gain, applied to the bus result on the way to the headphone
    /// device. Deliberately NOT part of processSample: §5.4 puts nothing on the
    /// bus but summing, trim and the safety limiter, so the -3 dBFS ceiling stays
    /// a property of the bus rather than of the current listening level.
    float applyMasterVolume (float busSample) const noexcept;

    /// Maps a -20..+20 dB trim value to a linear multiplier.
    static float trimDbToLinearGain (float trimDb) noexcept;

    /// True once the runaway cut has engaged; stays true until manuallyUnmute() is called.
    bool isRunawayMuted() const noexcept { return runawayMuted; }
    void manuallyUnmute() noexcept;

    /// Global instantaneous mute (spacebar), independent of runaway cut.
    void setGlobalMute (bool shouldMute) noexcept { globallyMuted = shouldMute; }
    bool isGloballyMuted() const noexcept { return globallyMuted; }

    bool isMuted() const noexcept { return globallyMuted || runawayMuted; }

    /// Feed a 1/3-octave-band analysis result (done outside the RT audio callback's
    /// hot path if it needs FFT work -- the detector here just tracks the growth
    /// rule). bandLevelDb is one band's level, broadbandPeakDb is the mix peak.
    bool processFeedbackCandidate (double bandLevelDb, double broadbandPeakDb, double blockSeconds) noexcept;

private:
    double sampleRate;
    bool limiterEngaged = false;
    double limiterEngagedSeconds = 0.0;
    bool runawayMuted = false;
    bool globallyMuted = false;
    double masterVolume = kDefaultMonitorVolume;

    // Feedback detector state: level at the start of the current growth window.
    double feedbackWindowStartDb = -200.0;
    double feedbackWindowElapsed = 0.0;
};

} // namespace mma
