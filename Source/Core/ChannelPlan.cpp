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
        const auto inputIsDisabled = [&d] (int input)
        {
            return std::find (d.disabledInputs.begin(), d.disabledInputs.end(), input)
                   != d.disabledInputs.end();
        };

        // Analysis is allowed to observe only a complete physical pair. If a
        // socket is switched off, inspecting it would make an unselected input
        // influence the verdict and could collapse away the selected one on
        // the next rebuild. A remembered Stereo answer is final, too.
        const bool needsStereoAnalysis = d.inputChannelCount == 2
                                      && ! d.hasChannelLayoutDecision
                                      && ! inputIsDisabled (0)
                                      && ! inputIsDisabled (1);

        // The name the user gave this port wins over the product string --
        // otherwise the strip says "Blue Yeti" while the files say "Kitchen".
        const std::string base = d.assignedName.empty() ? d.productName : d.assignedName;

        for (int input = 0; input < inputs; ++input)
        {
            // A socket switched off in Settings is not recorded at all. The
            // ones left keep their physical numbers -- input 2 is still the
            // socket labelled 2 on the box, whatever happened to input 1.
            if (inputIsDisabled (input))
                continue;

            PlannedChannel c;
            c.deviceKey = d.deviceKey;
            c.deviceChannel = input;
            c.collapseStereoPair = d.inputChannelCount == 2
                                && d.knownDuplicateStereo;
            c.analyzeStereoPair = needsStereoAnalysis && input == 0;
            c.monoSourceChannel = d.monoSourceChannel == 1 ? 1 : 0;

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
