#include "AsioOutputBackend.h"

#if JUCE_WINDOWS
#include <windows.h>

namespace mma {

AsioOutputBackend::AsioOutputBackend() = default;
AsioOutputBackend::~AsioOutputBackend() { deactivate(); }

bool AsioOutputBackend::checkAsioRegistryEntry() const
{
    // TODO: query HKEY_LOCAL_MACHINE\SOFTWARE\ASIO for our CLSID to confirm
    // self-registration succeeded on a prior install/first-run.
    HKEY key;
    const bool present = RegOpenKeyExA (HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ, &key) == ERROR_SUCCESS;
    if (present)
        RegCloseKey (key);
    return present;
}

bool AsioOutputBackend::isAvailable() const
{
    return checkAsioRegistryEntry();
}

bool AsioOutputBackend::activate()
{
    // TODO: instantiate/register the IASIO COM object, expose buffer-switch.
    driverRegistryPresent = checkAsioRegistryEntry();
    registered = driverRegistryPresent;
    return registered;
}

void AsioOutputBackend::deactivate()
{
    registered = false;
}

VirtualDeviceBackendStatus AsioOutputBackend::getStatus() const
{
    VirtualDeviceBackendStatus status;
    status.kind = VirtualDeviceBackendKind::AsioOutput;
    status.available = driverRegistryPresent;
    status.active = registered;
    status.reachDescription = "DAWs and streaming software that support ASIO (e.g. OBS) can see this "
                               "as an input. Web browsers and video-call apps cannot.";
    status.limitationsDescription = "ASIO-only. No Microsoft signing required, but not visible outside "
                                     "ASIO-aware applications.";
    return status;
}

void AsioOutputBackend::writeMixBlock (const float* /*interleaved*/, int /*numFrames*/, int /*numChannels*/)
{
    // TODO: copy into the current ASIOBufferInfo output buffers from the
    // most recent bufferSwitch() callback.
}

} // namespace mma

#endif // JUCE_WINDOWS
