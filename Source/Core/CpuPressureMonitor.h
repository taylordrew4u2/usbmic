#pragma once

namespace mma {

enum class PerformanceWarning
{
    None,
    /// §6.6: sustained CPU pressure above 80% for 30 seconds. Warned before it
    /// causes dropouts, not after.
    SustainedCpuPressure,
    /// §6.6: the OS reports thermal throttling. Dropouts follow shortly.
    ThermalThrottling,
};

/// §6.6 performance headroom. Warning early is the whole point: once the
/// callback is already missing deadlines the take is damaged, and §0.1 puts
/// not losing audio first.
class CpuPressureMonitor
{
public:
    static constexpr double kPressureThreshold = 0.80;
    static constexpr double kSustainedSeconds = 30.0;

    /// cpuLoad is 0..1. Returns a warning the first time the load has been over
    /// threshold continuously for the full window; a dip below threshold
    /// restarts the clock, so a brief spike never warns.
    PerformanceWarning update (double cpuLoad, bool thermallyThrottled, double nowSeconds);

    void reset() noexcept;

    /// Seconds the load has been continuously over threshold, for the Advanced
    /// panel. Zero when it is not.
    double getSecondsOverThreshold (double nowSeconds) const noexcept;

private:
    bool overThreshold = false;
    double overThresholdSince = 0.0;
    bool warnedCpu = false;
    bool warnedThermal = false;
};

} // namespace mma
