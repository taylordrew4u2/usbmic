#include "CpuPressureMonitor.h"

namespace mma {

PerformanceWarning CpuPressureMonitor::update (double cpuLoad, bool thermallyThrottled, double nowSeconds)
{
    // Thermal throttling is reported by the OS rather than inferred, so it does
    // not need a sustain window -- it is already the machine telling us it has
    // slowed down.
    if (thermallyThrottled)
    {
        if (! warnedThermal)
        {
            warnedThermal = true;
            return PerformanceWarning::ThermalThrottling;
        }
    }
    else
    {
        warnedThermal = false;
    }

    if (cpuLoad > kPressureThreshold)
    {
        if (! overThreshold)
        {
            overThreshold = true;
            overThresholdSince = nowSeconds;
        }
        else if (! warnedCpu && (nowSeconds - overThresholdSince) >= kSustainedSeconds)
        {
            warnedCpu = true;
            return PerformanceWarning::SustainedCpuPressure;
        }
    }
    else
    {
        // A dip below threshold restarts the clock: §6.6 asks for sustained
        // pressure, and a momentary spike is not that.
        overThreshold = false;
        warnedCpu = false;
    }

    return PerformanceWarning::None;
}

double CpuPressureMonitor::getSecondsOverThreshold (double nowSeconds) const noexcept
{
    return overThreshold ? (nowSeconds - overThresholdSince) : 0.0;
}

void CpuPressureMonitor::reset() noexcept
{
    overThreshold = false;
    overThresholdSince = 0.0;
    warnedCpu = false;
    warnedThermal = false;
}

} // namespace mma
