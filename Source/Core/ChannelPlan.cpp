#include "ChannelPlan.h"
#include <algorithm>
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
            // A socket switched off in Settings is not recorded at all. The
            // ones left keep their physical numbers -- input 2 is still the
            // socket labelled 2 on the box, whatever happened to input 1.
            if (std::find (d.disabledInputs.begin(), d.disabledInputs.end(), input)
                != d.disabledInputs.end())
                continue;

            PlannedChannel c;
            c.deviceKey = d.deviceKey;
            c.deviceChannel = input;

            // A name given to this input names the person on it, and needs no
            // socket number to be told apart.
            const auto named = d.inputNames.find (input);
            c.displayName = named != d.inputNames.end() && ! named->second.empty()
                          ? named->second
                          : plannedChannelName (base, input, inputs);

            channels.push_back (std::move (c));
        }
    }

    return channels;
}

} // namespace mma
