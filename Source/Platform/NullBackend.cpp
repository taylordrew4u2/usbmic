#include "NullBackend.h"

namespace mma {

VirtualDeviceBackendStatus NullBackend::getStatus() const
{
    VirtualDeviceBackendStatus status;
    status.kind = VirtualDeviceBackendKind::None;
    status.available = true;
    status.active = true;
    status.reachDescription = "No other application can see this as a microphone. "
                               "Recording and monitoring in this app both work normally.";
    status.limitationsDescription = "Not visible to any other application, conferencing tool, or DAW.";
    return status;
}

void NullBackend::writeMixBlock (const float*, int, int)
{
    // Nothing to do: there is no virtual endpoint to feed.
}

} // namespace mma
