#pragma once
#include "VirtualDeviceBackend.h"

#if JUCE_WINDOWS

namespace mma {

/// §7 backend C: bundle a licensed, signed third-party virtual-audio-cable
/// driver (VB-Audio, Virtual Audio Cable, Thesycon) and write the mix into
/// its endpoint. This is the intended production path for reaching every
/// Windows application, but it requires a commercial per-seat license
/// agreement with the driver vendor -- not something this codebase can
/// obtain or ship. This class is a deliberate, clearly-marked stub: the
/// interface is real and matches what activation/writing would look like,
/// but activate() always fails until a real licensed driver package is
/// integrated (installer bundling, EULA acceptance flow, per-seat license
/// key handling all still need to be built once that agreement exists).
class LicensedVirtualCableBackend : public VirtualDeviceBackend
{
public:
    VirtualDeviceBackendKind getKind() const override { return VirtualDeviceBackendKind::LicensedVirtualCable; }

    bool isAvailable() const override { return false; } // gated on licensing, see class doc

    bool activate() override { return false; }
    void deactivate() override {}

    VirtualDeviceBackendStatus getStatus() const override
    {
        VirtualDeviceBackendStatus status;
        status.kind = VirtualDeviceBackendKind::LicensedVirtualCable;
        status.available = false;
        status.active = false;
        status.reachDescription = "Not yet available: every Windows application would be able to see "
                                   "this as a microphone once a licensed driver is bundled.";
        status.limitationsDescription = "Requires a commercial per-seat license from a virtual-cable "
                                         "driver vendor (VB-Audio, Virtual Audio Cable, or Thesycon). "
                                         "Not implemented -- this is a licensing/business step, not an "
                                         "engineering one.";
        return status;
    }

    void writeMixBlock (const float*, int, int) override {}
};

} // namespace mma

#endif // JUCE_WINDOWS
