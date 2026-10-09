#pragma once
#include <algorithm>
#include <chrono>

namespace mma {

/// Real time between calls, for UI-timer-driven timing.
///
/// The meters' ballistics, peak hold, tap-to-name and every §10.5 setup
/// detector are timed from the UI timer, and were handed a fixed 1/60 s (or
/// 0.5 s) per callback on the assumption that the timer fires exactly at its
/// rate. It does not: App Nap throttles an occluded app's timers to a few a
/// second, and a busy message thread delivers them late and coalesced. A
/// 2-second peak hold then lasted half a minute, the 300 ms tap-to-name
/// sustain several seconds, and the 40-second "nothing has ever arrived"
/// advice minutes. Each caller now asks how long it has really been.
///
/// One step is capped (maxStepSeconds): a machine that slept for an hour has
/// not watched a silent rig for an hour, and a detector should not be handed
/// it as one observation.
class ElapsedClock
{
public:
    using NowFn = double (*)();

    explicit ElapsedClock (double firstStepSeconds, double maxStepSeconds = 5.0, NowFn nowFn = nullptr) noexcept
        : firstStep (firstStepSeconds), maxStep (maxStepSeconds), now (nowFn != nullptr ? nowFn : &steadySeconds)
    {
    }

    /// Seconds since the previous tick(), clamped to [0, maxStepSeconds]; the
    /// first call returns firstStepSeconds, there being nothing to measure
    /// from.
    double tick() noexcept
    {
        const double t = now();
        const double step = ticked ? std::clamp (t - last, 0.0, maxStep) : firstStep;
        last = t;
        ticked = true;
        return step;
    }

    /// True when at least `periodSeconds` has passed since the last tick(),
    /// or there has not been one: a cadence that follows the clock rather than
    /// counting callbacks.
    bool isDue (double periodSeconds) const noexcept
    {
        return ! ticked || now() - last >= periodSeconds;
    }

    static double steadySeconds() noexcept
    {
        return std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
    }

private:
    double firstStep;
    double maxStep;
    NowFn now;
    double last = 0.0;
    bool ticked = false;
};

} // namespace mma
