#pragma once
#include <string>
#include <vector>

namespace mma {

/// Where a take is going, and how loud that place wants it.
///
/// Every streaming service normalises what it is given to one integrated
/// loudness figure, so the number decides what a listener actually hears. A
/// take delivered louder than the target is turned down; one delivered quieter
/// is either turned up or simply played quiet, depending on the platform. So
/// "as loud as possible" has not been the right answer for a decade -- what it
/// buys is a turn-down and the dynamics already crushed to get there.
///
/// The targets here are the platforms' own published figures, not a house
/// guess. Spotify states -14 LUFS with true peak below -1 dBTP; Apple Podcasts
/// states -16 LUFS +/-1 with the same ceiling.
struct StreamingTarget
{
    std::string name;

    /// Integrated loudness the platform normalises to, in LUFS, as the platform
    /// publishes it -- which is referenced to stereo. See monoTargetLufs().
    double stereoTargetLufs;

    /// The ceiling the platform asks true peaks to stay under, in dBTP.
    double truePeakCeilingDbtp;

    /// One line on what this platform does with the number, in plain language.
    std::string note;
};

/// The platforms worth offering, best-known first.
const std::vector<StreamingTarget>& streamingTargets();

/// Look-up by name. Null when there is no such target.
const StreamingTarget* findStreamingTarget (const std::string& name);

/// The target to actually aim a *mono* file at, which is 3 LU below the
/// published figure.
///
/// This is the correction that matters most here and is easiest to get wrong,
/// because every file this app writes is mono. A mono file played back through
/// both speakers is the same signal twice, and BS.1770 measures that pair 3 LU
/// louder than the single channel on its own. So a mono file delivered at the
/// published -16 plays back at -13: three decibels hotter than everything
/// around it, on every platform, every time.
///
/// A mono file at -19 LUFS sounds as loud as a stereo file at -16.
double monoTargetLufs (const StreamingTarget& target);

/// What one take needs, measured against one platform.
struct LoudnessAdvice
{
    /// Gain to apply to hit the target, in dB. Positive means turn up.
    double gainDb = 0.0;

    /// True once the take is loud enough that applying `gainDb` would push its
    /// true peak past the platform's ceiling. Then the gain is reduced to
    /// whatever the ceiling allows, and this says so -- because the alternative
    /// is handing someone a file that meets the loudness target by clipping.
    bool limitedByTruePeak = false;

    /// Nothing to measure yet: too short, or silent.
    bool measurable = false;

    /// The take is already within this many LU of the target, so moving it
    /// would be fiddling rather than fixing.
    bool alreadyOnTarget = false;

    /// Plain-language summary, in the app's voice. Never empty.
    std::string summary;
};

/// Closer than this to the target and the take is called on target. A
/// half-decibel is below what anyone can hear on a voice, and chasing it
/// would have the advice change every time someone clears their throat.
inline constexpr double kOnTargetToleranceLu = 0.5;

/// Loudness under which a take is too short or too quiet to judge.
inline constexpr int kMinimumBlocksToJudge = 30; // 3 seconds of 100 ms hops

/// Works out what to do with one take for one platform.
///
/// `measuredLufs` and `measuredTruePeakDbtp` come from LoudnessMeter over the
/// mix. `blockCount` is that meter's block count, so a take too short to judge
/// is reported as such rather than given a confident and meaningless number.
///
/// Pure: no audio, no files, no platform. All of the judgement, none of the
/// machinery.
LoudnessAdvice adviseForTarget (const StreamingTarget& target,
                                double measuredLufs,
                                double measuredTruePeakDbtp,
                                int blockCount);

} // namespace mma
