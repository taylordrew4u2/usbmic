#include "DeviceManager.h"
#include <algorithm>

namespace mma {

bool DeviceManager::addDevice (MicDeviceState device)
{
    // Reject a device we already know. This used to append unconditionally, so
    // a caller that re-enumerated -- which is exactly what a hotplug
    // notification does -- got a second copy of every device already present.
    for (const auto& existing : devices)
        if (existing.identity.key() == device.identity.key())
            return false;

    // The caller owns enumerationOrder on this path -- that is the documented
    // contract and several callers rely on it. Only keep the internal counter
    // ahead of it, so devices added later by syncToEnumeration (which does
    // assign its own) cannot collide with one supplied here.
    nextEnumerationOrder = std::max (nextEnumerationOrder, device.enumerationOrder + 1);

    devices.push_back (std::move (device));
    reapplyCapacityLimit();
    return true;
}

bool DeviceManager::setUserEnabled (const std::string& identityKey, bool enabled)
{
    for (auto& d : devices)
    {
        if (d.identity.key() != identityKey)
            continue;

        if (d.userEnabled == enabled)
            return false;

        d.userEnabled = enabled;
        reapplyCapacityLimit();
        return true;
    }

    return false;
}

bool DeviceManager::syncToEnumeration (const std::vector<MicDeviceState>& seen)
{
    bool changed = false;

    // Drop anything the OS no longer reports.
    const auto stillPresent = [&seen] (const MicDeviceState& d)
    {
        for (const auto& s : seen)
            if (s.identity.key() == d.identity.key())
                return true;
        return false;
    };

    const auto before = devices.size();
    devices.erase (std::remove_if (devices.begin(), devices.end(),
                                   [&] (const MicDeviceState& d) { return ! stillPresent (d); }),
                   devices.end());
    changed = (devices.size() != before);

    for (const auto& s : seen)
    {
        auto it = std::find_if (devices.begin(), devices.end(), [&] (const MicDeviceState& d) {
            return d.identity.key() == s.identity.key();
        });

        if (it == devices.end())
        {
            MicDeviceState added = s;
            added.enumerationOrder = nextEnumerationOrder++;
            devices.push_back (std::move (added));
            changed = true;
            continue;
        }

        // Already known: refresh only what the OS owns. Everything the app has
        // learned since -- enumeration order, drift history, inclusion, the
        // name the user typed -- survives, because a device that never left is
        // not a new device.
        if (it->displayName != s.displayName)
        {
            it->displayName = s.displayName;
            changed = true;
        }

        it->isBuiltIn = s.isBuiltIn;
    }

    if (changed)
        reapplyCapacityLimit();

    return changed;
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
    return true; // §3.3 failover follows from the erase -- see removeDevice()'s note
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
        // A microphone the user has cleared is excluded outright and does not
        // consume one of the eight slots -- deselecting a mic you are not using
        // should make room for one you are.
        if (! d->userEnabled)
        {
            d->included = false;
            d->exclusionReason = "Not selected. Tick it in Settings to record it.";
            continue;
        }

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

std::vector<const MicDeviceState*> DeviceManager::rankMasterCandidates() const
{
    std::vector<const MicDeviceState*> ranked;
    ranked.reserve (devices.size());

    // §3.1: an explicit choice wins over the lowest-drift rule, but only while
    // that device is actually present and included -- otherwise the rig would
    // have no master at all.
    const MicDeviceState* preferred = nullptr;

    if (! preferredMasterKey.empty())
        for (const auto& d : devices)
            if (d.included && d.identity.key() == preferredMasterKey)
                preferred = &d;

    if (preferred != nullptr)
        ranked.push_back (preferred);

    // §3.1: prefer something the user plugged in. A machine's built-in
    // microphone is a legitimate input but a poor timebase, and CoreAudio
    // enumerates it first, so before any drift measurement exists it would win
    // on enumeration order alone -- which is why the clock master read as the
    // computer on every Mac regardless of how many USB mics were attached.
    //
    // Built-ins are not dropped, only ranked last: one is still a better
    // timebase than none at all.
    for (int pass = 0; pass < 2; ++pass)
    {
        std::vector<const MicDeviceState*> group;

        for (const auto& d : devices)
        {
            if (! d.included || &d == preferred)
                continue;
            if (d.isBuiltIn != (pass == 1))
                continue;

            group.push_back (&d);
        }

        std::stable_sort (group.begin(), group.end(),
                          [] (const MicDeviceState* a, const MicDeviceState* b)
                          { return lowerDrift (*a, *b); });

        ranked.insert (ranked.end(), group.begin(), group.end());
    }

    return ranked;
}

const MicDeviceState* DeviceManager::selectDefaultMaster() const
{
    // One ordering, used both here and by the mid-take failover, so the rule a
    // take falls back to is the same rule that chose the master to begin with.
    const auto ranked = rankMasterCandidates();
    return ranked.empty() ? nullptr : ranked.front();
}

} // namespace mma
