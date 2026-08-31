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

    /// True for a machine's own built-in microphone rather than something the
    /// user plugged in. §3.1 needs this: a built-in mic is a fine input but a
    /// poor timebase, and CoreAudio enumerates it first, so without this the
    /// computer wins master selection on every Mac.
    bool isBuiltIn = false;

    /// §1 caps the rig at 8; this is the user's own choice on top of that.
    /// Deselecting a microphone excludes it from capture, monitoring, metering
    /// and drift exactly as the cap does, because `included` is the single flag
    /// every one of those paths already consults.
    bool userEnabled = true;
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
    ///
    /// Returns false and changes nothing if a device with this identity is
    /// already known. It used to append unconditionally, which meant every
    /// re-enumeration added a second copy of every device already in the list.
    bool addDevice (MicDeviceState device);

    /// Reconciles the whole list against what the OS currently reports: adds
    /// devices that are new, drops devices that are gone, and leaves devices
    /// that are still present exactly as they are -- same enumeration order,
    /// same drift history, same inclusion.
    ///
    /// This is what a device-change notification must call. Looping over the
    /// enumeration calling addDevice() is what produced five copies of one
    /// microphone: macOS fires its device-list listener several times while a
    /// USB mic initialises, and each firing re-added every device already
    /// present.
    ///
    /// Returns true if the set of devices actually changed, so callers can skip
    /// the work a no-op notification does not require.
    bool syncToEnumeration (const std::vector<MicDeviceState>& seen);

    /// Removes a device (unplug). Returns true if the device was found.
    ///
    /// §3.3 failover needs nothing further: the device is gone from the list,
    /// so selectDefaultMaster() can no longer return it and already promotes
    /// the lowest-drift survivor -- while still honouring the user's override
    /// and still declining to hand the clock to a built-in microphone.
    bool removeDevice (const PortIdentity& identity);

    const std::vector<MicDeviceState>& getDevices() const { return devices; }

    /// §3.1: default master is the included device with the lowest measured
    /// drift once available; before any measurement exists, enumeration order.
    /// Returns nullptr if there are no included devices.
    const MicDeviceState* selectDefaultMaster() const;

    /// §3.1: the user may override the automatic choice from the Advanced
    /// panel. The override holds until that device leaves the rig, at which
    /// point §3.3 failover takes over again from the automatic rule.
    void setPreferredMaster (const std::string& identityKey);

    /// §3.1: drift is only claimed once 60 seconds of running measurement
    /// exists -- before that the loop is still settling and the number is
    /// noise. The caller reports elapsed measurement time so this class owns
    /// that rule rather than each caller reimplementing it.
    static constexpr double kDriftMeasurementSeconds = 60.0;
    void updateMeasuredDrift (const std::string& identityKey, double driftPpm,
                              double measuredForSeconds);
    const std::string& getPreferredMaster() const { return preferredMasterKey; }

    /// Re-evaluates the 8-mic-cap inclusion flags after an add/remove, in
    /// enumeration order (earliest 8 included mics win; opt-out is never used
    /// to pick a favorite -- purely first-come per §1).
    void reapplyCapacityLimit();

    /// The user ticking or clearing a microphone in the Advanced panel.
    /// Returns true if the flag actually changed, so the caller can avoid
    /// tearing down and rebuilding the audio streams for a no-op.
    bool setUserEnabled (const std::string& identityKey, bool enabled);

private:
    std::vector<MicDeviceState> devices;
    std::string preferredMasterKey;

    // Monotonic, so a device unplugged and replugged sorts after the ones that
    // stayed rather than reclaiming its old priority.
    int nextEnumerationOrder = 0;

    static bool lowerDrift (const MicDeviceState& a, const MicDeviceState& b);
};

} // namespace mma
