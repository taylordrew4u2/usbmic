#include "FailedOpenRetry.h"

#include <algorithm>

namespace mma {

bool FailedOpenRetry::tick (double elapsedSeconds, const Situation& situation) noexcept
{
    const bool stuck = situation.idle
                    && situation.includedMicCount > 0
                    && ! situation.monitoring
                    && ! situation.permissionDenied;

    if (! stuck)
    {
        // Anything that is not "stuck" ends the episode. The next failure is a
        // new problem and is retried promptly, not after the long wait an
        // earlier one had backed off to.
        interval = kFirstIntervalSeconds;
        remaining = kFirstIntervalSeconds;
        return false;
    }

    remaining -= std::max (0.0, elapsedSeconds);

    // A hair of slack so a poll that lands on the boundary in floating point
    // fires on that tick, not the next.
    if (remaining > 1.0e-9)
        return false;

    interval = std::min (kMaxIntervalSeconds, interval * 2.0);
    remaining = interval;
    return true;
}

} // namespace mma
