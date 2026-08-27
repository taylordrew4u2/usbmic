#pragma once

// Defines JUCE_MAC / JUCE_WINDOWS -- see the note in IAudioBackend.h.
#include <juce_core/juce_core.h>

#include <string>
#include <vector>

namespace mma {

/// §7: the app-facing virtual-device output strategy. Which applications can
/// see the aggregate device depends entirely on which backend is active; the
/// UI must always show this plainly (§7: "names, in plain language, which
/// applications can and cannot see the aggregate device").
enum class VirtualDeviceBackendKind
{
    None,                 // A: standalone recorder+monitor, ships immediately
    AsioOutput,            // B: user-mode ASIO DLL, no MS signing, DAWs/OBS only
    LicensedVirtualCable,  // C: bundled signed 3rd-party driver, intended production path
    AttestationSignedDriver // D: own attestation-signed WDM driver, longest lead item
};

struct VirtualDeviceBackendStatus
{
    VirtualDeviceBackendKind kind = VirtualDeviceBackendKind::None;
    bool available = false;
    bool active = false;
    std::string reachDescription;  // plain-language: which apps can see it
    std::string limitationsDescription;
};

/// Common interface for backends B/C/D (and the no-op A). §7's swappable
/// backend behind one interface.
class VirtualDeviceBackend
{
public:
    virtual ~VirtualDeviceBackend() = default;

    virtual VirtualDeviceBackendKind getKind() const = 0;

    /// True if this backend's prerequisites (driver installed/registered,
    /// license present, etc.) are currently satisfied on this machine.
    virtual bool isAvailable() const = 0;

    /// Attempts to activate the backend (register the virtual endpoint, open
    /// the write path into it). Returns false with no partial side effects on
    /// failure.
    virtual bool activate() = 0;

    virtual void deactivate() = 0;

    virtual VirtualDeviceBackendStatus getStatus() const = 0;

    /// Writes one block of the summed mix to the virtual endpoint, once active.
    virtual void writeMixBlock (const float* interleaved, int numFrames, int numChannels) = 0;
};

} // namespace mma
