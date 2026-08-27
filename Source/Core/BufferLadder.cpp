#include "BufferLadder.h"
#include <algorithm>

namespace mma {

void BufferLadder::dropOverrunsBefore (double cutoffSeconds)
{
    recentOverruns.erase (std::remove_if (recentOverruns.begin(), recentOverruns.end(),
                                          [cutoffSeconds] (double t) { return t < cutoffSeconds; }),
                          recentOverruns.end());
}

bool BufferLadder::noteOverrun (double nowSeconds)
{
    recentOverruns.push_back (nowSeconds);
    dropOverrunsBefore (nowSeconds - kWindowSeconds);

    if (static_cast<int> (recentOverruns.size()) < kOverrunTrigger || isAtMaximum())
        return false;

    const int from = getCurrentSize();
    ++currentIndex;
    changeLog.push_back ({ nowSeconds, from, getCurrentSize() });

    // The window starts fresh at the new size: overruns at the old size say
    // nothing about whether the new one is big enough.
    recentOverruns.clear();
    return true;
}

bool BufferLadder::resetToLowest() noexcept
{
    if (isRecording)
        return false;

    currentIndex = 0;
    recentOverruns.clear();
    return true;
}

} // namespace mma
