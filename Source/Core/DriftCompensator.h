#pragma once

namespace mma {

/// PI-loop ASRC ratio controller for one input, per spec §3.2. Drives a
/// resample ratio from measured ring-buffer fill error so a device's clock is
/// continuously pulled into sync with the clock consuming it, without audible
/// pitch jumps (bounded slew) or excursions beyond a safety clamp.
///
/// Every input gets one, the §3.1 clock master included -- see §3.1's note on
/// why the master is a reporting reference rather than an exemption.
class DriftCompensator
{
public:
    static constexpr double kKp = 1.0e-6;              // proportional gain, per sample of fill error
    static constexpr double kKi = 1.0e-8;               // integral gain, per sample of fill error
    static constexpr double kMaxRatioDeviationPpm = 200.0; // clamp: max +/-200ppm ratio deviation
    static constexpr double kMaxSlewPpmPerSecond = 5.0;     // never correct instantaneously

    explicit DriftCompensator (double sampleRate) noexcept;

    void reset() noexcept;

    /// Feed one control-loop update. fillError is (measured fill samples - target fill samples);
    /// positive means the buffer is filling faster than it drains (device running fast).
    /// blockSizeSamples is the number of samples represented by this update, used to convert
    /// the PPM/second slew limit into a per-call step limit.
    void update (double fillError, int blockSizeSamples) noexcept;

    /// Current resample ratio to apply to this stream, e.g. 1.0 + deviation.
    double getRatio() const noexcept { return 1.0 + currentPpm * 1.0e-6; }

    /// Current deviation from unity, in parts-per-million. Used for §3.3 drift reporting.
    double getPpm() const noexcept { return currentPpm; }

    /// True once |ppm| has stayed above 100 for a sustained period (§3.3 "unreliable" flag).
    bool isSustainedExcessDrift() const noexcept { return sustainedExcessDrift; }

    /// Feed elapsed wall-clock seconds since the last drift-flag check (call once per reporting tick).
    /// referencePpm is the clock master's own deviation: §3.3 judges a device against the master, so
    /// what is tested is this loop's PPM relative to that. It defaults to zero, which judges against
    /// the stream that pulls this one instead.
    void updateSustainedDriftFlag (double elapsedSeconds, double referencePpm = 0.0) noexcept;

private:
    double sampleRate;
    double integralTerm = 0.0;
    double currentPpm = 0.0;
    double excessDriftSeconds = 0.0;
    bool sustainedExcessDrift = false;

    static constexpr double kExcessDriftThresholdPpm = 100.0;
    // Spec says "100 PPM sustained" without naming a duration; §3.3 reports drift every 10s,
    // so we sustain-check on that same cadence (judgment call, documented in README).
    static constexpr double kExcessDriftSustainSeconds = 10.0;
};

} // namespace mma
