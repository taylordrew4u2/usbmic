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

    // The rates a device can actually do: what it advertises, plus the rate it
    // is running at right now.
    //
    // A device running at 44.1 kHz supports 44.1 kHz, whatever its advertised
    // list says -- it is doing it. Some interfaces report only the rate they
    // would prefer, and taking that list as the whole truth ruled out the one
    // rate guaranteed to work.
    auto ratesFor = [] (const DeviceRateCapability& d)
    {
        std::set<uint32_t> rates;

        for (auto rate : d.supportedRates)
            if (rate <= kRateCap)
                rates.insert (rate);

        if (d.currentRate != 0 && d.currentRate <= kRateCap)
            rates.insert (d.currentRate);

        return rates;
    };

    // Rates <= 48kHz common to every device.
    std::set<uint32_t> common = ratesFor (devices.front());

    for (size_t i = 1; i < devices.size(); ++i)
    {
        const std::set<uint32_t> deviceRates = ratesFor (devices[i]);

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

std::vector<DeviceRateCapability>
    SampleRateNegotiator::votingDevices (const std::vector<std::string>& includedDeviceKeys,
                                         const std::vector<EnumeratedDeviceRates>& enumerated)
{
    std::vector<DeviceRateCapability> out;
    int index = 0;
    for (const auto& key : includedDeviceKeys)
    {
        const auto found = std::find_if (enumerated.begin(), enumerated.end(),
                                         [&key] (const EnumeratedDeviceRates& e)
                                         { return e.deviceKey == key; });
        if (found == enumerated.end())
            continue;
        DeviceRateCapability cap;
        cap.deviceIndex = index++;
        cap.supportedRates = found->supportedRates;
        cap.currentRate = found->currentRate;
        out.push_back (std::move (cap));
    }
    return out;
}

} // namespace mma