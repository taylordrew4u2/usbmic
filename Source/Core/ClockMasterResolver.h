#pragma once

#include <string>
#include <vector>

namespace mma {

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
/// but is writing silence. Locking the rig's timebase to either one would
/// resample every other microphone onto a clock that carries no audio.
MasterResolution resolveMasterChannel (const std::vector<std::string>& takeChannelIds,
                                       const std::vector<bool>& channelLive,
                                       const std::vector<std::string>& rankedCandidateIds);

} // namespace mma
