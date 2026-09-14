#include "DeadChannelDetector.h"
#include <algorithm>

namespace mma {

DeadChannelDetector::DeadChannelDetector (int numChannelsIn)
    : numChannels (numChannelsIn),
      belowThresholdSeconds (static_cast<size_t> (numChannelsIn), 0.0),
      anotherChannelWasActiveDuringWindow (static_cast<size_t> (numChannelsIn), false),
      dead (static_cast<size_t> (numChannelsIn), false)
{
}

void DeadChannelDetector::reset()
{
    std::fill (belowThresholdSeconds.begin(), belowThresholdSeconds.end(), 0.0);
    std::fill (anotherChannelWasActiveDuringWindow.begin(), anotherChannelWasActiveDuringWindow.end(), false);
    std::fill (dead.begin(), dead.end(), false);
}

void DeadChannelDetector::processBlock (const std::vector<float>& peaksDb, double blockSeconds)
{
    if (static_cast<int> (peaksDb.size()) != numChannels)
        return;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        // "Another channel" active means any channel other than this one crossed -40dBFS.
        bool otherChannelActiveThisBlock = false;
        for (int other = 0; other < numChannels; ++other)
        {
            if (other != ch && peaksDb[static_cast<size_t> (other)] > kOtherActiveThresholdDb)
            {
                otherChannelActiveThisBlock = true;
                break;
            }
        }

        // <=, not <, and the difference is the whole detector.
        //
        // Metering FLOORS at its kMinDb, which is this same -60 dBFS: both the
        // dB conversion (std::max (kMinDb, ...)) and the peak hold (std::clamp
        // (..., kMinDb, kMaxDb)) stop there. So a channel with NO signal at all
        // reports exactly -60.0, and "< -60.0" is false for it forever. The one
        // condition this detector exists to catch -- §10.5's "a silent channel
        // is most often the hardware mute switch, the single most common
        // failure" -- was the one condition it could not see.
        //
        // It passed its own tests because every one of them feeds -70 dBFS, a
        // value the real metering cannot produce. A silent channel is at the
        // floor, and at the floor is what the spec means by below.
        if (peaksDb[static_cast<size_t> (ch)] <= kDeadThresholdDb)
        {
            belowThresholdSeconds[static_cast<size_t> (ch)] += blockSeconds;
            if (otherChannelActiveThisBlock)
                anotherChannelWasActiveDuringWindow[static_cast<size_t> (ch)] = true;

            if (belowThresholdSeconds[static_cast<size_t> (ch)] >= kSustainSeconds
                && anotherChannelWasActiveDuringWindow[static_cast<size_t> (ch)])
            {
                dead[static_cast<size_t> (ch)] = true;
            }
        }
        else
        {
            belowThresholdSeconds[static_cast<size_t> (ch)] = 0.0;
            anotherChannelWasActiveDuringWindow[static_cast<size_t> (ch)] = false;
            dead[static_cast<size_t> (ch)] = false;
        }
    }
}

bool DeadChannelDetector::isChannelDead (int channelIndex) const
{
    if (channelIndex < 0 || channelIndex >= numChannels)
        return false;
    return dead[static_cast<size_t> (channelIndex)];
}

} // namespace mma
