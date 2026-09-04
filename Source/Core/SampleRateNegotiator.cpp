#include "SampleRateNegotiator.h"
#include <algorithm>
#include <iterator> // std::inserter: MSVC's STL does not pull this in via <set>
#include <set>

namespace mma {

RateNegotiationResult SampleRateNegotiator::negotiate (const std::vector<DeviceRateCapability>& devices)
{
    RateNegotiationResult result;

    if (devices.empty())
    {
        result.chosenRate = kRateCap;
        return result;
    }

    // Rates <= 48kHz common to every device.
    std::set<uint32_t> common;
    for (auto rate : devices.front().supportedRates)
        if (rate <= kRateCap)
            common.insert (rate);

    for (size_t i = 1; i < devices.size(); ++i)
    {
        std::set<uint32_t> deviceRates;
        for (auto rate : devices[i].supportedRates)
            if (rate <= kRateCap)
                deviceRates.insert (rate);

        std::set<uint32_t> intersection;
        std::set_intersection (common.begin(), common.end(),
                               deviceRates.begin(), deviceRates.end(),
                               std::inserter (intersection, intersection.begin()));
        common = std::move (intersection);
    }

    if (! common.empty())
    {
        // A rate every device is already running at, if there is one and it is
        // common. Switching is the step that fails: an interface clock-locked
        // to 44.1 kHz lists 48 kHz and then refuses the write, and the take
        // never starts. Staying put cannot fail.
        uint32_t alreadyRunning = 0;

        for (const auto& d : devices)
        {
            if (d.currentRate == 0 || common.count (d.currentRate) == 0)
            {
                alreadyRunning = 0;
                break;
            }

            if (alreadyRunning == 0)
                alreadyRunning = d.currentRate;
            else if (alreadyRunning != d.currentRate)
            {
                // The rig disagrees with itself, so something must move
                // whatever we pick. Fall through to highest-common and let §3
                // resample the ones that cannot follow.
                alreadyRunning = 0;
                break;
            }
        }

        // Highest common rate, capped at 48kHz, unless the whole rig is already
        // sitting on one.
        result.chosenRate = alreadyRunning != 0 ? alreadyRunning : *common.rbegin();
    }
    else
    {
        // No rate is common to all devices under the cap. Never reject a microphone
        // for rate reasons: fall back to the highest rate the most-limited device can
        // reach (capped), so at least one device is native and the rest resample.
        uint32_t best = 0;
        for (const auto& d : devices)
        {
            uint32_t deviceBest = 0;
            for (auto rate : d.supportedRates)
                if (rate <= kRateCap)
                    deviceBest = std::max (deviceBest, rate);
            if (deviceBest == 0 && ! d.supportedRates.empty())
                deviceBest = *std::min_element (d.supportedRates.begin(), d.supportedRates.end());
            best = std::max (best, deviceBest);
        }
        result.chosenRate = (best > 0) ? std::min (best, kRateCap) : kRateCap;
    }

    for (const auto& d : devices)
    {
        const bool nativelySupported = std::find (d.supportedRates.begin(), d.supportedRates.end(), result.chosenRate)
                                        != d.supportedRates.end();
        if (! nativelySupported)
            result.devicesNeedingResample.push_back (d.deviceIndex);
    }

    return result;
}

} // namespace mma
