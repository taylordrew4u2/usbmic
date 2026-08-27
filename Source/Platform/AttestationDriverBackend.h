#pragma once
#include "VirtualDeviceBackend.h"
#include "PlatformMacros.h"

#if JUCE_WINDOWS

namespace mma {

/// §7 backend D: our own attestation-signed WDM/KS kernel-mode driver. This
/// is the longest-lead item in the whole product -- registered legal entity,
/// EV code-signing certificate, Windows Hardware Dev Center Partner Center
/// account, and CAB submission for attestation signing, per §7. None of that
/// is a code problem, and none of it can be done from this codebase: it's
/// legal-entity registration and a signing pipeline outside the repo. This
/// class documents the shape of what activation would eventually do; it is
/// intentionally never functional here.
class AttestationDriverBackend : public VirtualDeviceBackend
{
public:
    VirtualDeviceBackendKind getKind() const override { return VirtualDeviceBackendKind::AttestationSignedDriver; }

    bool isAvailable() const override { return false; } // gated on EV cert + Partner Center, see class doc

    bool activate() override { return false; }
    void deactivate() override {}

    VirtualDeviceBackendStatus getStatus() const override
    {
        VirtualDeviceBackendStatus status;
        status.kind = VirtualDeviceBackendKind::AttestationSignedDriver;
        status.available = false;
        status.active = false;
        status.reachDescription = "Not yet available: once signed, every Windows application would see "
                                   "this as a microphone, on Windows 10/11 desktop only (not Windows "
                                   "Server, which ignores attestation signing).";
        status.limitationsDescription = "Requires a registered legal entity, an EV code-signing "
                                         "certificate, a Microsoft Partner Center account, and CAB "
                                         "submission for attestation signing. Attestation-signed is not "
                                         "WHQL and cannot be distributed via Windows Update. Administrative "
                                         "lead time, not engineering -- start this paperwork on day one "
                                         "per §7, run it in parallel with everything else.";
        return status;
    }

    void writeMixBlock (const float*, int, int) override {}

    // TODO once the legal/signing path is complete: implement the actual
    // WDM/KS miniport driver (a kernel-mode .sys, built and submitted through
    // the Windows Hardware Dev Center attestation flow) and the user-mode
    // side that writes the mix into its KS pin. Nothing here should be
    // attempted without the EV certificate and Partner Center registration
    // already in hand.
};

} // namespace mma

#endif // JUCE_WINDOWS
