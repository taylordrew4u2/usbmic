#include "DeviceManager.h"
#include <algorithm>

namespace mma {

void DeviceManager::addDevice (MicDeviceState device)
{
    devices.push_back (std::move (device));
    reapplyCapacityLimit();
}

bool DeviceManager::removeDevice (const PortIdentity& identity)
{
    auto it = std::find_if (devices.begin(), devices.end(), [&] (const MicDeviceState& d) {
        return d.identity.key() == identity.key();
    });

    if (it == devices.end())
        return false;

    devices.erase (it);
    reapplyCapacityLimit();
    return true; // caller determines master-ness externally and calls selectFailoverMaster as needed
}

void DeviceManager::reapplyCapacityLimit()
{
    // Included status is decided strictly by enumeration order among ALL known
    // devices (not just currently-included ones), so the first 8 ever seen keep
    // priority even if a later one has since been unplugged and replugged.
    std::vector<MicDeviceState*> byOrder;
    byOrder.reserve (devices.size());
    for (auto& d : devices)
        byOrder.push_back (&d);
    std::sort (byOrder.begin(), byOrder.end(), [] (const MicDeviceState* a, const MicDeviceState* b) {
        return a->enumerationOrder < b->enumerationOrder;
    });

    int includedCount = 0;
    for (auto* d : byOrder)
    {
        if (includedCount < kMaxMicrophones)
        {
            d->included = true;
            d->exclusionReason.clear();
            ++includedCount;
        }
        else
        {
            d->included = false;
            d->exclusionReason = "More than 8 microphones are connected. This one is shown but not recorded.";
        }
    }
}

bool DeviceManager::lowerDrift (const MicDeviceState& a, const MicDeviceState& b)
{
    // Devices with a real measurement always sort ahead of those without one.
    if (a.hasDriftMeasurement != b.hasDriftMeasurement)
        return a.hasDriftMeasurement; // a wins (sorts "lower") if it has a measurement and b doesn't
    if (a.hasDriftMeasurement && b.hasDriftMeasurement)
    {
        if (a.measuredDriftPpm != b.measuredDriftPpm)
            return a.measuredDriftPpm < b.measuredDriftPpm;
    }
    return a.enumerationOrder < b.enumerationOrder; // tiebreak
}

void DeviceManager::setPreferredMaster (const std::string& identityKey)
{
    preferredMasterKey = identityKey;
}

void DeviceManager::updateMeasuredDrift (const std::string& identityKey, double driftPpm,
                                         double measuredForSeconds)
{
    for (auto& d : devices)
    {
        if (d.identity.key() != identityKey)
            continue;

        d.measuredDriftPpm = driftPpm;

        // §3.1: reporting a number before the window has elapsed would make
        // master selection chase a settling loop.
        d.hasDriftMeasurement = measuredForSeconds >= kDriftMeasurementSeconds;
        return;
    }
}

const MicDeviceState* DeviceManager::selectDefaultMaster() const
{
    // §3.1: an explicit choice wins over the lowest-drift rule, but only while
    // that device is actually present and included -- otherwise the rig would
    // have no master at all.
    if (! preferredMasterKey.empty())
        for (const auto& d : devices)
            if (d.included && d.identity.key() == preferredMasterKey)
                return &d;

    const MicDeviceState* best = nullptr;
    for (const auto& d : devices)
    {
        if (! d.included)
            continue;
        if (best == nullptr || lowerDrift (d, *best))
            best = &d;
    }
    return best;
}

const MicDeviceState* DeviceManager::selectFailoverMaster (const PortIdentity& removedMasterIdentity) const
{
    const MicDeviceState* best = nullptr;
    for (const auto& d : devices)
    {
        if (! d.included)
            continue;
        if (d.identity.key() == removedMasterIdentity.key())
            continue;
        if (best == nullptr || lowerDrift (d, *best))
            best = &d;
    }
    return best;
}

} // namespace mma
