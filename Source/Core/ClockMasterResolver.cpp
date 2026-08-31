#include "ClockMasterResolver.h"

namespace mma {

MasterResolution resolveMasterChannel (const std::vector<std::string>& takeChannelIds,
                                       const std::vector<bool>& channelLive,
                                       const std::vector<std::string>& rankedCandidateIds)
{
    for (const auto& candidate : rankedCandidateIds)
    {
        if (candidate.empty())
            continue;

        for (size_t i = 0; i < takeChannelIds.size(); ++i)
        {
            if (takeChannelIds[i] != candidate)
                continue;

            // Present in the take, but dead: skip to the next candidate rather
            // than returning it. §3.3 wants the recording to keep running on a
            // working timebase, and silence is not one.
            if (i < channelLive.size() && ! channelLive[i])
                break;

            return { static_cast<int> (i), candidate };
        }
    }

    // §3.3 tolerates a bounded transient but not a stopped recording, so the
    // caller is told "no master" (free-running) rather than being handed a
    // channel that cannot serve as one.
    return {};
}

} // namespace mma
