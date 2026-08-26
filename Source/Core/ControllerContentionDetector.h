#pragma once
#include <string>
#include <vector>

namespace mma {

/// §14.3: warns, before arming, when a USB card reader shares a controller
/// with the microphones -- a common cause of dropouts that otherwise present
/// as an unexplained application fault.
class ControllerContentionDetector
{
public:
    /// controllerId is an OS-reported USB host-controller identifier; empty/unknown
    /// means the OS didn't expose topology and we can't judge co-location.
    struct DeviceControllerInfo
    {
        std::string deviceId;
        std::string controllerId;
        bool isCardReader = false;
        bool isMicrophone = false;
    };

    /// Returns true (and fills outReason) if a card reader and at least one
    /// microphone share a controller.
    static bool detectContention (const std::vector<DeviceControllerInfo>& devices, std::string& outReason);
};

} // namespace mma
