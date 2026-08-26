#pragma once
#include "VirtualDeviceBackend.h"

namespace mma {

/// §7 backend A: standalone recorder + monitor, no virtual device at all.
/// Always available on every platform; see NullBackend.cpp for the (trivial)
/// implementation.
class NullBackend : public VirtualDeviceBackend
{
public:
    VirtualDeviceBackendKind getKind() const override { return VirtualDeviceBackendKind::None; }
    bool isAvailable() const override { return true; }
    bool activate() override { return true; }
    void deactivate() override {}
    VirtualDeviceBackendStatus getStatus() const override;
    void writeMixBlock (const float* interleaved, int numFrames, int numChannels) override;
};

} // namespace mma
