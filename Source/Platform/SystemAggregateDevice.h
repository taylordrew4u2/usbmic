#pragma once
#include <memory>
#include <string>
#include <vector>

namespace mma {

/// The outward face of the rig: one combined, user-named input device that
/// OTHER apps can record from, containing every connected microphone as its
/// channels. This is what makes the aggregator feel like a single device
/// rather than a pile of identical USB mics.
///
/// On macOS this is a real CoreAudio aggregate device created through the
/// public HAL API -- no driver, no signing, visible system-wide in every app's
/// input list. Windows has no public equivalent, which is exactly why the §7
/// driver backends (B/C/D) exist; there the status string says so plainly.
class SystemAggregateDevice
{
public:
    virtual ~SystemAggregateDevice() = default;

    /// Creates or replaces the combined device. deviceUids are the platform
    /// device identifiers in channel order; masterUid is the §3.1 clock source
    /// (the OS resamples every other sub-device onto it). An empty list
    /// removes the device -- no mics means nothing to show other apps.
    virtual bool publish (const std::string& name,
                          const std::vector<std::string>& deviceUids,
                          const std::string& masterUid) = 0;

    /// Takes the combined device back out of the system.
    virtual void remove() = 0;

    /// Plain language for the Advanced panel: what other apps currently see.
    virtual std::string getStatus() const = 0;
};

/// The right implementation for this platform. Never null; on platforms with
/// no public aggregate API it returns a stub whose status says why.
std::unique_ptr<SystemAggregateDevice> createSystemAggregateDevice();

} // namespace mma
