#include "OutputDeviceSelector.h"
#include <algorithm>

namespace mma {

bool OutputDeviceSelector::isEligible (const OutputDeviceCandidate& candidate)
{
    return ! candidate.isMicrophonePlaybackEndpoint && ! candidate.isAlsoSelectedInput;
}

OutputSelection OutputDeviceSelector::select (const std::vector<OutputDeviceCandidate>& candidates,
                                              const std::string& rememberedId)
{
    OutputSelection result;

    std::vector<const OutputDeviceCandidate*> eligible;
    for (const auto& c : candidates)
        if (isEligible (c))
            eligible.push_back (&c);

    if (eligible.empty())
    {
        result.explanation = candidates.empty()
            ? "No headphones found. Plug headphones into the computer, or into a headphone amp connected to it."
            : "The only sound outputs are the microphones themselves. Plug headphones into the computer or a headphone amp instead, or you'll hear yourself twice.";
        return result;
    }

    auto pick = [&result] (const OutputDeviceCandidate* c, OutputSelectionReason reason)
    {
        result.found = true;
        result.id = c->id;
        result.reason = reason;
    };

    // 1. What the user chose before, if it is present.
    if (! rememberedId.empty())
    {
        auto it = std::find_if (eligible.begin(), eligible.end(),
                                [&] (const OutputDeviceCandidate* c) { return c->id == rememberedId; });

        if (it != eligible.end())
        {
            pick (*it, OutputSelectionReason::RememberedFromPreviousSession);
            return result;
        }
    }

    // 2. Something plugged in after launch, most recent first.
    const OutputDeviceCandidate* newest = nullptr;
    for (auto* c : eligible)
        if (c->appearedAfterLaunch && (newest == nullptr || c->connectionOrder > newest->connectionOrder))
            newest = c;

    if (newest != nullptr)
    {
        pick (newest, OutputSelectionReason::NewlyConnected);
        return result;
    }

    // 3. Anything with a real headphone jack.
    auto jack = std::find_if (eligible.begin(), eligible.end(),
                              [] (const OutputDeviceCandidate* c) { return c->hasPhysicalHeadphoneJack; });

    if (jack != eligible.end())
    {
        pick (*jack, OutputSelectionReason::PhysicalHeadphoneJack);
        return result;
    }

    // 4. Whatever the OS considers default.
    auto def = std::find_if (eligible.begin(), eligible.end(),
                             [] (const OutputDeviceCandidate* c) { return c->isSystemDefault; });

    if (def != eligible.end())
    {
        pick (*def, OutputSelectionReason::SystemDefault);
        return result;
    }

    // Eligible devices exist but none matched a stated priority. Take the first
    // rather than leaving the room without a monitor mix (§5.1: live from launch).
    pick (eligible.front(), OutputSelectionReason::SystemDefault);
    return result;
}

} // namespace mma
