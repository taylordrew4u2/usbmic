#pragma once

namespace mma {

/// What the write pipeline should do at the current ring-buffer fill (§6.5).
enum class WritePipelineState
{
    Healthy,
    /// 50% fill: warn visually. Never silently drop (§0.1).
    FillWarning,
    /// 90% fill with no mirror to fall back on: write the mix file only and log
    /// the exact sample position where that started.
    DegradedToMixOnly,
};

/// Which remaining-time warning has just become due, if any. Each fires once.
enum class RemainingTimeWarning
{
    None,
    TenMinutes,
    TwoMinutes,
    Exhausted,
};

/// §6.5 capacity and back-pressure. Kept separate from the writer so it can be
/// tested without touching a filesystem, and so the audio callback never has to
/// reason about thresholds.
class CapacityMonitor
{
public:
    static constexpr double kFillWarningFraction = 0.50;
    static constexpr double kFillDegradeFraction = 0.90;
    static constexpr double kTenMinutesSeconds = 600.0;
    static constexpr double kTwoMinutesSeconds = 120.0;

    /// mirrorAvailable is false once the mirror has been stopped for space
    /// (§6.3) or was never enabled. Degrading to mix-only is only correct when
    /// there is no second copy to protect the stems.
    WritePipelineState evaluateFill (double fillFraction, bool mirrorAvailable) noexcept;

    /// Call with the current remaining recording time. Returns a warning the
    /// first time each threshold is crossed and None thereafter, so the UI is
    /// not re-warned every frame.
    RemainingTimeWarning evaluateRemaining (double remainingSeconds) noexcept;

    bool hasWarnedTenMinutes() const noexcept { return warnedTenMinutes; }
    bool hasWarnedTwoMinutes() const noexcept { return warnedTwoMinutes; }

    /// Sample position where mix-only degradation began, or -1 if it never did.
    long long getDegradationSamplePosition() const noexcept { return degradationSamplePosition; }
    void noteDegradationAt (long long samplePosition) noexcept;

    void reset() noexcept;

private:
    bool warnedTenMinutes = false;
    bool warnedTwoMinutes = false;
    bool warnedExhausted = false;
    long long degradationSamplePosition = -1;
};

} // namespace mma
