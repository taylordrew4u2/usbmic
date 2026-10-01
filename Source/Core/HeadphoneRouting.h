#pragma once
#include <functional>
#include <string>
#include <vector>

namespace mma {

/// One sub-device of the combined device, in the order its output channels
/// appear: the device's id and how many output channels it brings.
struct CombinedDeviceOutputs
{
    std::string deviceId;
    int outputChannels = 0;
};

/// The gain for every output channel of the combined device: 1 for a channel
/// on a microphone whose wearer hears the mix, 0 for one whose headphones are
/// switched off. The combined device lays its outputs out sub-device after
/// sub-device, so a microphone's jack is the run of channels it brings.
///
/// Everyone who hears anything hears the same mix (§5.2) -- this only decides
/// whose headphones it reaches.
inline std::vector<float> headphoneChannelGains (const std::vector<CombinedDeviceOutputs>& layout,
                                                 const std::function<bool (const std::string&)>& hearsMix)
{
    std::vector<float> gains;

    for (const auto& device : layout)
    {
        const float gain = (hearsMix == nullptr || hearsMix (device.deviceId)) ? 1.0f : 0.0f;

        for (int ch = 0; ch < device.outputChannels; ++ch)
            gains.push_back (gain);
    }

    return gains;
}

/// True when at least one microphone in the combined device has a headphone
/// jack to send the mix to. A rig of microphones with no outputs has nothing
/// for the combined device to play through.
inline bool combinedDeviceHasHeadphones (const std::vector<CombinedDeviceOutputs>& layout)
{
    for (const auto& device : layout)
        if (device.outputChannels > 0)
            return true;

    return false;
}

} // namespace mma
