#pragma once
#include <string>
#include <vector>

namespace mma {

/// One device, as the channel plan needs to see it. Deliberately plain data:
/// this header is reachable from the headless tests, and the app's own device
/// record is not.
struct ChannelPlanDevice
{
    std::string deviceKey;
    /// The hardware's own name.
    std::string productName;
    /// §2.4's remembered name for this port. Empty when the user has not named it.
    std::string assignedName;
    int inputChannelCount = 1;
    /// §2.4's remembered §2.1 verdict: true only once the analyzer has decided
    /// the two sides carry the same source.
    bool knownDuplicateStereo = false;
};

/// One take channel: a strip on screen, and a file on disk. The two must agree,
/// which is the whole reason this is computed once rather than in each place.
struct PlannedChannel
{
    std::string deviceKey;
    int deviceChannel = 0;
    std::string displayName;
};

/// The name one input of a device is known by, on screen and in its filename.
///
/// An interface's inputs all carry the device's name, which would give four
/// identical strips over four differently-named files. The input number is what
/// tells them apart, and it is the number printed next to the socket the
/// microphone is plugged into. A device contributing a single channel is a
/// microphone rather than an interface, and numbering it would be noise.
std::string plannedChannelName (const std::string& baseName,
                                int deviceChannel,
                                int channelsFromThisDevice);

/// Every channel the given devices produce, in take order.
///
/// This is the single description of what a rig records. It was previously
/// written out twice -- once in the app's capture builder, per input, and once
/// again in the screen's accessors, per device -- and the two disagreed. A
/// two-input interface recorded two files while the screen showed one
/// microphone until the moment recording began, which reads exactly like an app
/// that cannot see the second microphone at all.
std::vector<PlannedChannel> planChannels (const std::vector<ChannelPlanDevice>& devices);

} // namespace mma
