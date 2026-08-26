#include "BusPowerDetector.h"

namespace mma {

void BusPowerDetector::recordEvent (double nowSeconds, int micsAttached)
{
    events.push_back ({ nowSeconds, micsAttached });
    prune (nowSeconds);
}

void BusPowerDetector::prune (double nowSeconds)
{
    while (! events.empty() && (nowSeconds - events.front().timestamp) > kWindowSeconds)
        events.pop_front();
}

bool BusPowerDetector::isBusPowerExhausted (double nowSeconds) const
{
    int count = 0;
    bool hadEnoughMics = false;
    for (const auto& e : events)
    {
        if ((nowSeconds - e.timestamp) <= kWindowSeconds)
        {
            ++count;
            if (e.micsAttached >= kMinMicsForConcern)
                hadEnoughMics = true;
        }
    }
    return count >= kEventThreshold && hadEnoughMics;
}

} // namespace mma
