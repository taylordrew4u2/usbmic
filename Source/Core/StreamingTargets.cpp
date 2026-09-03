#include "StreamingTargets.h"
#include "LoudnessMeter.h"
#include <algorithm>
#include <cmath>

namespace mma {

namespace {

/// A mono file played through both speakers is the same signal twice, and
/// BS.1770 measures that pair 3.01 LU louder than the one channel alone.
constexpr double kMonoToStereoLu = 3.01;

std::string oneDecimal (double value)
{
    // Built by hand rather than through snprintf's "%.1f", which honours the C
    // locale: on a machine set to one that writes a comma, every number in this
    // advice would come out with a separator the rest of the app does not use.
    const bool negative = value < 0.0;
    const auto tenths = static_cast<long long> (std::llround (std::abs (value) * 10.0));

    return (negative && tenths != 0 ? "-" : "")
         + std::to_string (tenths / 10) + "." + std::to_string (tenths % 10);
}

} // namespace

const std::vector<StreamingTarget>& streamingTargets()
{
    // Each platform's own published figure. Where a platform states a range,
    // the centre of it is used and the note says so.
    static const std::vector<StreamingTarget> targets = {
        { "Spotify", -14.0, -1.0,
          "Turns anything louder down to -14. Going louder buys nothing but a "
          "turn-down and the dynamics you crushed to get there." },

        { "YouTube", -14.0, -1.0,
          "Normalises to about -14. Quieter uploads are left quiet rather than "
          "turned up, so undershooting costs you." },

        { "Apple Podcasts", -16.0, -1.0,
          "Asks for -16 give or take a decibel. The usual home for spoken word." },

        { "Apple Music", -16.0, -1.0,
          "Sound Check normalises to -16, a little quieter than the rest." },

        { "Amazon Music", -14.0, -1.0, "Normalises to -14, like Spotify." },

        { "Tidal", -14.0, -1.0, "Normalises to -14, like Spotify." },

        { "Broadcast (EBU R128)", -23.0, -1.0,
          "What television and radio deliver to. Much quieter than streaming, "
          "and not what you want unless someone has asked for it." },
    };

    return targets;
}

const StreamingTarget* findStreamingTarget (const std::string& name)
{
    for (const auto& target : streamingTargets())
        if (target.name == name)
            return &target;

    return nullptr;
}

double monoTargetLufs (const StreamingTarget& target)
{
    return target.stereoTargetLufs - kMonoToStereoLu;
}

LoudnessAdvice adviseForTarget (const StreamingTarget& target,
                                double measuredLufs,
                                double measuredTruePeakDbtp,
                                int blockCount)
{
    LoudnessAdvice advice;

    if (blockCount < kMinimumBlocksToJudge || measuredLufs <= LoudnessMeter::kAbsoluteGateLufs)
    {
        advice.summary = "Not enough sound yet to judge how loud this is.";
        return advice;
    }

    advice.measurable = true;

    const double wanted = monoTargetLufs (target);
    double gain = wanted - measuredLufs;

    if (std::abs (gain) <= kOnTargetToleranceLu)
    {
        advice.alreadyOnTarget = true;
        advice.gainDb = 0.0;
        advice.summary = "Already about right for " + target.name + ".";
        return advice;
    }

    // A gain that would put the true peak over the platform's ceiling is not a
    // gain worth offering: hitting the loudness target by clipping trades a
    // number the platform will fix anyway for distortion it cannot.
    const double headroom = target.truePeakCeilingDbtp - measuredTruePeakDbtp;

    if (gain > headroom)
    {
        gain = headroom;
        advice.limitedByTruePeak = true;
    }

    advice.gainDb = gain;

    const std::string direction = gain >= 0.0 ? "up" : "down";
    const std::string amount = oneDecimal (std::abs (gain));

    if (advice.limitedByTruePeak)
    {
        advice.summary = "Quieter than " + target.name + " wants, but there's only "
                       + amount + " dB of headroom before the peaks clip. Turn up by "
                       + amount + " dB and record the next take a little louder.";
    }
    else
    {
        advice.summary = "Turn " + direction + " by " + amount + " dB for " + target.name + ".";
    }

    // The mono correction is the thing nobody expects, so it is said rather
    // than silently applied -- someone checking this against a meter that
    // assumes stereo needs to know why the two disagree.
    advice.summary += " (Mono, so the aim is " + oneDecimal (wanted) + " LUFS, not "
                    + oneDecimal (target.stereoTargetLufs) + ".)";

    return advice;
}

} // namespace mma
