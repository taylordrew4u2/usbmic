#pragma once
#include <memory>
#include <string>
#include <vector>
#include "BusPowerDetector.h"
#include "ControllerContentionDetector.h"
#include "DeadChannelDetector.h"
#include "PolarPatternDetector.h"

namespace mma {

enum class SetupIssue
{
    BusPowerExhausted,    // §14.2
    ControllerContention, // §14.3
    NonCardioidPattern,   // §14.4
    SilentChannel,        // §8.1 / §10.5 -- usually the hardware mute switch
};

struct SetupAdvice
{
    SetupIssue issue;

    /// §10.6: what happened, then what to do, in one sentence. No codes, no
    /// apologies. The channel is named rather than numbered where one applies.
    std::string message;

    /// Channel this is about, or -1 when it concerns the whole rig.
    int channelIndex = -1;
};

/// §10.5 physical setup guidance. Novices fail on hardware, not software, so
/// the detectors for those failures are only worth having if something runs
/// them and turns them into a sentence. This is that something.
class SetupAdvisor
{
public:
    /// §14.2: an enumeration failure or a device dropping off the bus.
    void noteDeviceDropout (double nowSeconds, int micsAttached);

    /// §14.3: called when the device list changes, with whatever controller
    /// topology the OS exposed. An empty list clears the warning.
    void updateControllerTopology (const std::vector<ControllerContentionDetector::DeviceControllerInfo>& devices);

    /// §8.1: per-block channel peaks, used to find channels that have gone
    /// silent while others are live.
    void updateChannelLevels (const std::vector<float>& peaksDb, double blockSeconds);

    /// §14.4: correlation between two channels plus a third channel's level,
    /// which together indicate a microphone set to something other than cardioid.
    void updatePolarPattern (float correlationAB, float thirdChannelPeakDb, double blockSeconds);

    /// Names used in messages, so advice says "Kitchen" rather than "channel 2".
    void setChannelNames (std::vector<std::string> names);

    /// Everything currently worth telling the user, most serious first.
    std::vector<SetupAdvice> getActiveAdvice (double nowSeconds) const;

    void reset();

private:
    BusPowerDetector busPower;
    PolarPatternDetector polarPattern;

    // Built lazily and rebuilt when the channel count changes, because the
    // detector is fixed-width and mics come and go (§2, hot-plug).
    std::unique_ptr<DeadChannelDetector> deadChannels;

    std::vector<std::string> channelNames;
    std::string contentionReason;
    int numChannels = 0;

    std::string nameFor (int channelIndex) const;
};

} // namespace mma
