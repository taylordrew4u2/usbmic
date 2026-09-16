#pragma once

#include <string>
#include <vector>

namespace mma {

/// NOT CURRENTLY WIRED. Read this before trusting the tests below it.
///
/// Application::applyClockMaster() hard-codes setMasterChannel(-1) -- the clock
/// master is the computer, always -- and that is a deliberate product decision,
/// explained where it is made. §3.2 already corrects every microphone onto the
/// output clock, so naming a microphone "master" only moved which crystal the
/// drift figures were quoted against, and handed the user a picker for a choice
/// with no audible consequence.
///
/// So nothing in Source/ calls resolveMasterChannel, and the same is true of
/// DeviceManager's selectDefaultMaster / rankMasterCandidates /
/// setPreferredMaster / getPreferredMaster. The logic and its test suite are
/// correct and are kept against the decision being revisited -- but a passing
/// test here says nothing about what the app does, and that is exactly the
/// impression this note exists to prevent.
///
/// §3.3 master failover, expressed over the *take's* channel list rather than
/// the device list.
///
/// These are two different index spaces, and outside a recording they happen to
/// agree, which is what made conflating them so easy. During a take they do not:
/// the take's channel list is deliberately frozen (§6.5 -- an unplugged mic
/// keeps its slot and writes silence, because renumbering mid-take would
/// corrupt every stem), while DeviceManager tracks what the OS currently
/// reports and drops anything unplugged. So the moment a microphone leaves,
/// "the third included device" and "channel 3 of this take" stop being the same
/// thing.
///
/// Resolving the master by device id against the frozen list is the only way to
/// name a channel that survives an unplug. A pure function so the rule can be
/// tested without a device, a driver, or a take.
struct MasterResolution
{
    int channelIndex = -1;      ///< Index into the take's channel list, or -1.
    std::string deviceId;       ///< The device that won, empty when none did.
};

/// Walks `rankedCandidateIds` best-first (DeviceManager's §3.1 ordering) and
/// returns the first candidate that is both *in* this take and still live.
///
/// A candidate can fail either test independently: a microphone plugged in
/// mid-take is present and healthy but is not in the take at all (§6.5 -- it
/// joins nothing until the next one), and an unplugged channel is in the take
/// but is writing silence.
///
/// Naming either one costs §3.3 its meaning rather than the take its alignment:
/// per §3.1 the master is the reference every device's PPM is quoted against,
/// not an exemption from correction, and a channel index that is out of the
/// take's range or writing silence quotes the whole rig against nothing.
MasterResolution resolveMasterChannel (const std::vector<std::string>& takeChannelIds,
                                       const std::vector<bool>& channelLive,
                                       const std::vector<std::string>& rankedCandidateIds);

} // namespace mma
