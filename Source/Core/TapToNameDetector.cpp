#include "TapToNameDetector.h"
#include <algorithm>

namespace mma {

TapToNameDetector::TapToNameDetector (int channels)
    : numChannels (std::max (0, channels)),
      qualifyingSeconds (static_cast<size_t> (std::max (0, channels)), 0.0)
{
}

TapResult TapToNameDetector::processBlock (const std::vector<float>& peaksDb, double blockSeconds)
{
    // A conclusive result stands until the caller resets, so the prompt does not
    // flicker while the user reaches for the keyboard.
    if (result != TapResult::Listening)
        return result;

    if (static_cast<int> (peaksDb.size()) != numChannels || numChannels == 0)
        return result;

    int loudCount = 0;
    int loudest = -1;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        if (peaksDb[static_cast<size_t> (ch)] > kTapThresholdDb)
        {
            ++loudCount;
            loudest = ch;
        }
    }

    // §14.6: two mics hearing the same tap cannot be told apart, and guessing
    // would name the wrong skull. Say so and let them try again.
    if (loudCount > 1)
    {
        result = TapResult::Ambiguous;
        return result;
    }

    if (loudCount == 0)
    {
        std::fill (qualifyingSeconds.begin(), qualifyingSeconds.end(), 0.0);
        return result;
    }

    // Exactly one channel is above the tap threshold. It only counts while every
    // other channel stays genuinely quiet.
    for (int ch = 0; ch < numChannels; ++ch)
    {
        if (ch == loudest)
            continue;

        if (peaksDb[static_cast<size_t> (ch)] >= kQuietThresholdDb)
        {
            std::fill (qualifyingSeconds.begin(), qualifyingSeconds.end(), 0.0);
            return result;
        }
    }

    auto& held = qualifyingSeconds[static_cast<size_t> (loudest)];
    held += blockSeconds;

    if (held >= kSustainSeconds)
    {
        result = TapResult::ChannelIdentified;
        tappedChannel = loudest;
    }

    return result;
}

void TapToNameDetector::reset()
{
    result = TapResult::Listening;
    tappedChannel = -1;
    std::fill (qualifyingSeconds.begin(), qualifyingSeconds.end(), 0.0);
}

} // namespace mma
