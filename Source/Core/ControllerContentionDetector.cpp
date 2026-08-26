#include "ControllerContentionDetector.h"

namespace mma {

bool ControllerContentionDetector::detectContention (const std::vector<DeviceControllerInfo>& devices, std::string& outReason)
{
    for (const auto& reader : devices)
    {
        if (! reader.isCardReader || reader.controllerId.empty())
            continue;

        for (const auto& mic : devices)
        {
            if (! mic.isMicrophone || mic.controllerId.empty())
                continue;

            if (mic.controllerId == reader.controllerId)
            {
                outReason = "Your card reader and microphones share one USB connection. "
                            "Use the built-in card slot if you have one.";
                return true;
            }
        }
    }
    return false;
}

} // namespace mma
