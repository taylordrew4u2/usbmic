#pragma once
#include "VirtualDeviceBackend.h"
#include "PlatformMacros.h"

#if JUCE_WINDOWS

namespace mma {

/// §7 backend B: a user-mode ASIO output DLL, COM-registered, requiring no
/// Microsoft signing. Reaches DAWs and OBS (anything with an ASIO driver
/// selector) but not browsers or conferencing apps, which don't speak ASIO.
/// This is a genuine architecture stub: writing a full ASIO SDK host-facing
/// driver (a COM object implementing IASIO, self-registering via regsvr32/
/// the registry CLSID under HKEY_LOCAL_MACHINE\SOFTWARE\ASIO) is a real
/// multi-week undertaking on its own and depends on the ASIO SDK (Steinberg
/// license, not bundled here). The class shape below is the real interface
/// this backend would implement; TODOs mark exactly what's missing.
class AsioOutputBackend : public VirtualDeviceBackend
{
public:
    AsioOutputBackend();
    ~AsioOutputBackend() override;

    VirtualDeviceBackendKind getKind() const override { return VirtualDeviceBackendKind::AsioOutput; }
    bool isAvailable() const override;
    bool activate() override;
    void deactivate() override;
    VirtualDeviceBackendStatus getStatus() const override;
    void writeMixBlock (const float* interleaved, int numFrames, int numChannels) override;

private:
    bool registered = false;
    bool driverRegistryPresent = false;

    // TODO: implement an IASIO-compatible COM object and self-register it
    // (DllRegisterServer under the ASIO registry key) so DAWs enumerate this
    // app as an ASIO driver. TODO: buffer-switch callback delivers the mix
    // block written via writeMixBlock() into ASIOBufferInfo buffers.
    bool checkAsioRegistryEntry() const;
};

} // namespace mma

#endif // JUCE_WINDOWS
