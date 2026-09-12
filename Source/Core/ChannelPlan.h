#pragma once
#include <map>
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
    /// Whether §2.1 has already made any decision for this port. A remembered
    /// Stereo verdict is just as final as a Mono one: without this separate
    /// bit, Stereo looks identical to a device that has never been observed
    /// and is analysed again on every launch.
    bool hasChannelLayoutDecision = false;
    /// §2.4's remembered §2.1 verdict: true only once the analyzer has decided
    /// the two sides carry the same source.
    bool knownDuplicateStereo = false;
    /// The analyzer-selected physical side for a remembered Mono verdict.
    int monoSourceChannel = 0;

    /// Physical inputs switched off in Settings. Not recorded, no strip, no
    /// file; the remaining inputs keep their socket numbers.
    std::vector<int> disabledInputs;

    /// A name per physical input, used verbatim in place of "<box> N".
    std::map<int, std::string> inputNames;
};

/// One take channel: a strip on screen, and a file on disk. The two must agree,
/// which is the whole reason this is computed once rather than in each place.
struct PlannedChannel
{
    std::string deviceKey;
    int deviceChannel = 0;
    std::string displayName;

    /// True only for a two-channel USB microphone whose two sides were
    /// previously confirmed to be one source. Interfaces and manually
    /// disabled sockets never set this: their physical-input routing must stay
    /// exact.
    bool collapseStereoPair = false;

    /// The first physical input of a fresh, fully enabled two-channel device
    /// owns §2.1's analyzer. Both planned channels continue to route exactly
    /// as selected while it observes the pair; only a later, persisted Mono
    /// verdict is allowed to collapse them on a rebuilt capture.
    bool analyzeStereoPair = false;

    /// Restored source for `collapseStereoPair`. Ignored unless that flag is
    /// true, and clamped again when the coordinator opens its streams.
    int monoSourceChannel = 0;
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
