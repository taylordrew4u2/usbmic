#include "SetupAdvisor.h"

namespace mma {

void SetupAdvisor::noteDeviceDropout (double nowSeconds, int micsAttached)
{
    busPower.recordEvent (nowSeconds, micsAttached);
}

void SetupAdvisor::updateControllerTopology (const std::vector<ControllerContentionDetector::DeviceControllerInfo>& devices)
{
    contentionReason.clear();

    std::string reason;
    if (ControllerContentionDetector::detectContention (devices, reason))
        contentionReason = reason;
}

void SetupAdvisor::updateChannelLevels (const std::vector<float>& peaksDb, double blockSeconds)
{
    const int incoming = static_cast<int> (peaksDb.size());

    // A hot-plug changes the channel count, and the detector is fixed-width.
    // Rebuilding drops the accumulated window, which is correct: the timings
    // measured against a different set of mics no longer describe this one.
    if (deadChannels == nullptr || incoming != numChannels)
    {
        numChannels = incoming;
        deadChannels = std::make_unique<DeadChannelDetector> (incoming);
    }

    if (incoming > 0)
        deadChannels->processBlock (peaksDb, blockSeconds);
}

void SetupAdvisor::updatePolarPattern (float correlationAB, float thirdChannelPeakDb, double blockSeconds)
{
    polarPattern.processBlock (correlationAB, thirdChannelPeakDb, blockSeconds);
}

void SetupAdvisor::setChannelNames (std::vector<std::string> names)
{
    channelNames = std::move (names);
}

std::string SetupAdvisor::nameFor (int channelIndex) const
{
    if (channelIndex >= 0 && channelIndex < static_cast<int> (channelNames.size())
        && ! channelNames[static_cast<size_t> (channelIndex)].empty())
        return channelNames[static_cast<size_t> (channelIndex)];

    return "Mic " + std::to_string (channelIndex + 1);
}

std::vector<SetupAdvice> SetupAdvisor::getActiveAdvice (double nowSeconds) const
{
    std::vector<SetupAdvice> advice;

    // Power first: it causes the dropouts and disappearing devices that
    // everything else here would otherwise be blamed for (§14.2).
    if (busPower.isBusPowerExhausted (nowSeconds))
        advice.push_back ({ SetupIssue::BusPowerExhausted,
                            "Your microphones need more power than this computer's USB port can supply. "
                            "Use a USB hub with its own power adapter.",
                            -1 });

    if (! contentionReason.empty())
        advice.push_back ({ SetupIssue::ControllerContention,
                            "Your card reader and microphones share one USB connection. "
                            "Use the built-in card slot if you have one.",
                            -1 });

    // §14.4: never say "bleed" -- the word means nothing to this user. Name the
    // knob and the setting instead.
    if (polarPattern.isTriggered())
        advice.push_back ({ SetupIssue::NonCardioidPattern,
                            "A microphone is picking up the whole room. "
                            "Turn its pattern knob to the single-heart setting.",
                            -1 });

    // §10.5: a silent channel is most often the hardware mute switch, which is
    // the single most common failure, so name it rather than being vague.
    for (int ch = 0; ch < numChannels; ++ch)
        if (deadChannels != nullptr && deadChannels->isChannelDead (ch))
            advice.push_back ({ SetupIssue::SilentChannel,
                                nameFor (ch) + " isn't sending sound. Check the mute button on the mic.",
                                ch });

    return advice;
}

void SetupAdvisor::reset()
{
    deadChannels.reset();
    polarPattern.reset();
    contentionReason.clear();
    numChannels = 0;
}

} // namespace mma
