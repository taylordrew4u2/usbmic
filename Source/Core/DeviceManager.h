#pragma once
#include "PortIdentity.h"
#include <string>
#include <vector>
#include <optional>

namespace mma {

struct MicDeviceState
{
    PortIdentity identity;
    std::string displayName;
    int enumerationOrder = 0;   // order first seen this session, for tiebreaks
    double measuredDriftPpm = 0.0;
    bool hasDriftMeasurement = false; // false until 60s of running measurement exists (§3.1)
    bool included = true;       // false only for the 9th+ mic, per §1
    std::string exclusionReason;
};

/// §1 8-mic cap, §3.1 master selection, §3.3 master failover. Pure logic over
/// an in-memory device list; actual OS enumeration/hotplug notifications are
/// wired in by the Platform layer and by App/RecordingEngine.
class DeviceManager
{
public:
    static constexpr int kMaxMicrophones = 8;

    /// Adds a newly-enumerated microphone. If this is the 9th+ mic (by
    /// enumeration order among currently-included mics), it is enumerated and
    /// displayed but excluded from capture with a stated reason (§1).
    void addDevice (MicDeviceState device);

    /// Removes a device (unplug). Returns true if the removed device was the
    /// current clock master, which the caller must react to via promoteFailoverMaster().
    bool removeDevice (const PortIdentity& identity);

    const std::vector<MicDeviceState>& getDevices() const { return devices; }

    /// §3.1: default master is the included device with the lowest measured
    /// drift once available; before any measurement exists, enumeration order.
    /// Returns nullptr if there are no included devices.
    const MicDeviceState* selectDefaultMaster() const;

    /// §3.3: on master unplug, promote the remaining device with the lowest
    /// measured drift; tiebreak by enumeration order. Returns nullptr if no
    /// devices remain.
    const MicDeviceState* selectFailoverMaster (const PortIdentity& removedMasterIdentity) const;

    /// Re-evaluates the 8-mic-cap inclusion flags after an add/remove, in
    /// enumeration order (earliest 8 included mics win; opt-out is never used
    /// to pick a favorite -- purely first-come per §1).
    void reapplyCapacityLimit();

private:
    std::vector<MicDeviceState> devices;

    static bool lowerDrift (const MicDeviceState& a, const MicDeviceState& b);
};

} // namespace mma
