#include "DeviceManager.h"
#include <algorithm>
#include <cmath>

namespace mma {

int takeChannelsForDevice (int inputChannelCount, bool knownDuplicateStereo) noexcept
{
    if (inputChannelCount <= 1)
        return 1;

    // The one case where collapsing is right, and only once §2.1 has looked at
    // the audio and said so. A guess is not evidence, and guessing wrong here
    // costs a person.
    if (inputChannelCount == 2 && knownDuplicateStereo)
        return 1;

    return inputChannelCount;
}

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

        // How many inputs the device presents is the OS's to say, and it has to
        // be refreshed here like any other fact the OS owns.
        //
        // Leaving it out meant an interface already in the list kept whatever
        // count it was first added with -- the default of 1 for any device that
        // arrived through addDevice() or was restored at startup -- so the app
        // went on believing a four-input interface had one microphone on it, and
        // the fix for exactly that bug never fired.
        if (it->inputChannelCount != s.inputChannelCount)
        {
            it->inputChannelCount = s.inputChannelCount;
            changed = true;
        }
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
        // §3.1's "lowest measured drift" is the smallest departure from
        // nominal in either direction. Compared signed, a device 150 PPM slow
        // sorted ahead of one dead on.
        const double aOff = std::abs (a.measuredDriftPpm), bOff = std::abs (b.measuredDriftPpm);
        if (aOff != bOff)
            return aOff < bOff;
    }
    return a.enumerationOrder < b.enumerationOrder; // tiebreak
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

} // namespace mma
