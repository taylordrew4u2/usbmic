#include "MonitorBus.h"
#include <algorithm>
#include <cmath>

namespace mma {

static_assert (std::atomic<bool>::is_always_lock_free,
               "MonitorBus requires lock-free bool atomics on the audio thread");
static_assert (std::atomic<float>::is_always_lock_free,
               "MonitorBus requires lock-free float atomics on the audio thread");
static_assert (std::atomic<double>::is_always_lock_free,
               "MonitorBus requires lock-free double atomics on the audio thread");

MonitorBus::MonitorBus (double sampleRateIn) noexcept
    : sampleRate (sampleRateIn),
      ceilingLinear (std::pow (10.0f, kLimiterCeilingDb / 20.0f)),
      masterGain (monitorVolumeToLinearGain (kDefaultMonitorVolume))
{
}

float MonitorBus::monitorVolumeToLinearGain (double volume0to100) noexcept
{
    const double v = std::clamp (volume0to100, 0.0, 100.0) / 100.0;
    if (v <= 0.0)
        return 0.0f;
    // Logarithmic (perceptual) mapping: -60dB at 0, 0dB at 100.
    constexpr double minDb = -60.0;
    const double db = minDb * (1.0 - v);
    return static_cast<float> (std::pow (10.0, db / 20.0));
}

float MonitorBus::trimDbToLinearGain (float trimDb) noexcept
{
    const float clamped = std::clamp (trimDb, static_cast<float> (kMinTrimDb), static_cast<float> (kMaxTrimDb));
    return std::pow (10.0f, clamped / 20.0f);
}

float MonitorBus::processSample (const std::vector<float>& trimmedInputSamples) noexcept
{
    // manuallyUnmute() is called from the message thread. Reset the remaining
    // limiter state here so those non-atomic counters stay audio-thread-owned.
    if (limiterResetRequested.load (std::memory_order_acquire)
        && limiterResetRequested.exchange (false, std::memory_order_acq_rel))
    {
        limiterEngagedSeconds = 0.0;
        limiterReleasedSeconds = 0.0;
        limiterEngaged = false;
    }

    if (isMuted())
        return 0.0f;

    float sum = 0.0f;
    for (auto s : trimmedInputSamples)
        sum += s; // unity sum, no per-channel attenuation with channel count

    // ceilingLinear is a member computed once at construction. It used to be a
    // std::pow evaluated on every sample of every block.
    const double sampleSeconds = (sampleRate > 0.0) ? (1.0 / sampleRate) : 0.0;

    float output = sum;
    bool engagedThisSample = false;

    if (std::abs (sum) > ceilingLinear)
    {
        // Zero-lookahead brickwall: clip instantly to the ceiling. No prediction,
        // no pre-gain-reduction -- accept the distortion, it only engages in a fault.
        output = std::copysign (ceilingLinear, sum);
        engagedThisSample = true;
    }

    if (engagedThisSample)
    {
        limiterEngaged = true;
        limiterReleasedSeconds = 0.0;
        limiterEngagedSeconds += sampleSeconds;

        if (limiterEngagedSeconds >= kRunawayCutSeconds)
            runawayMuted.store (true, std::memory_order_release);
    }
    else if (limiterEngaged)
    {
        // §5: the runaway cut is for 500 ms of *continuous* engagement, and
        // kLimiterReleaseSeconds is what "continuous" tolerates -- a gap shorter
        // than the 1 ms release is the same engagement, a longer one ends it.
        //
        // This previously drained the accumulator one sample-time per quiet
        // sample and never consulted kLimiterReleaseSeconds at all, so unwinding
        // an engagement took as long as building it. An engagement that had
        // ended seconds ago still carried most of its credit into the next,
        // unrelated one: two 300 ms bursts 10 ms apart -- neither close to the
        // threshold -- combined past 500 ms and cut the monitor. That is a false
        // alarm that silences someone's headphones mid-take and needs a manual
        // unmute, so a gap longer than the release now ends the run outright.
        limiterReleasedSeconds += sampleSeconds;

        if (limiterReleasedSeconds >= kLimiterReleaseSeconds)
        {
            limiterEngaged = false;
            limiterEngagedSeconds = 0.0;
            limiterReleasedSeconds = 0.0;
        }
    }

    if (isRunawayMuted())
        return 0.0f;

    return output;
}

void MonitorBus::setMasterVolume (double volume0to100) noexcept
{
    const auto clamped = std::max (0.0, std::min (100.0, volume0to100));
    masterVolume.store (clamped, std::memory_order_relaxed);

    // One aligned float store the callback picks up on its next sample, instead
    // of a std::pow per sample inside applyMasterVolume.
    masterGain.store (monitorVolumeToLinearGain (clamped), std::memory_order_release);
}

float MonitorBus::applyMasterVolume (float busSample) const noexcept
{
    return busSample * masterGain.load (std::memory_order_acquire);
}

void MonitorBus::manuallyUnmute() noexcept
{
    // The counters are owned by the audio callback. Ask it to reset them on
    // its next sample instead of racing it from the message thread.
    limiterResetRequested.store (true, std::memory_order_release);
    runawayMuted.store (false, std::memory_order_release);
}

bool MonitorBus::processFeedbackCandidate (double bandLevelDb, double broadbandPeakDb, double blockSeconds) noexcept
{
    const bool withinBroadband = (broadbandPeakDb - bandLevelDb) <= kFeedbackWithinBroadbandDb;

    if (! withinBroadband)
    {
        feedbackWindowStartDb = bandLevelDb;
        feedbackWindowElapsed = 0.0;
        return false;
    }

    feedbackWindowElapsed += blockSeconds;

    if (feedbackWindowElapsed >= kFeedbackWindowSeconds)
    {
        const bool grew = (bandLevelDb - feedbackWindowStartDb) > kFeedbackBandRiseDb;
        // Slide the window forward regardless, using this block as the new baseline.
        feedbackWindowStartDb = bandLevelDb;
        feedbackWindowElapsed = 0.0;

        if (grew)
        {
            runawayMuted.store (true, std::memory_order_release); // visible reason is supplied by the UI layer
            return true;
        }
    }

    return false;
}

} // namespace mma
