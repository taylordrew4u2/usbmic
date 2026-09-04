#include "ChannelPlan.h"
#include "DeviceManager.h"

namespace mma {

std::string plannedChannelName (const std::string& baseName,
                                int deviceChannel,
                                int channelsFromThisDevice)
{
    if (channelsFromThisDevice <= 1)
        return baseName;

    return baseName + " " + std::to_string (deviceChannel + 1);
}

std::vector<PlannedChannel> planChannels (const std::vector<ChannelPlanDevice>& devices)
{
    std::vector<PlannedChannel> channels;

    for (const auto& d : devices)
    {
        const int inputs = takeChannelsForDevice (d.inputChannelCount, d.knownDuplicateStereo);

        // The name the user gave this port wins over the product string --
        // otherwise the strip says "Blue Yeti" while the files say "Kitchen".
        const std::string base = d.assignedName.empty() ? d.productName : d.assignedName;

        for (int input = 0; input < inputs; ++input)
        {
            PlannedChannel c;
            c.deviceKey = d.deviceKey;
            c.deviceChannel = input;
            c.displayName = plannedChannelName (base, input, inputs);
            channels.push_back (std::move (c));
        }
    }

    return channels;
}

} // namespace mma
