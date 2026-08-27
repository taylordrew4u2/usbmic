#pragma once
#include <vector>

namespace mma {

enum class TapResult
{
    /// Nothing conclusive yet. Keep feeding blocks.
    Listening,
    /// Exactly one channel was heard clearly; getTappedChannel() names it.
    ChannelIdentified,
    /// Two or more channels heard the same tap, so it cannot be attributed.
    /// §14.6: "Two mics heard that -- try tapping closer to one."
    Ambiguous,
};

/// §14.6 tap-to-name. Four Yetis enumerate with the same product string, so a
/// novice cannot tell which skull is which person -- the spec calls this a
/// first-run blocker. The user taps one microphone and it names itself.
class TapToNameDetector
{
public:
    static constexpr float kTapThresholdDb = -25.0f;
    static constexpr float kQuietThresholdDb = -45.0f;
    static constexpr double kSustainSeconds = 0.300;

    explicit TapToNameDetector (int numChannels);

    /// Feed one block of per-channel peaks. Returns what can be concluded so
    /// far. A conclusive result latches until reset(), so the caller can show
    /// the prompt without racing the audio thread.
    TapResult processBlock (const std::vector<float>& peaksDb, double blockSeconds);

    TapResult getResult() const noexcept { return result; }

    /// Valid only when the result is ChannelIdentified.
    int getTappedChannel() const noexcept { return tappedChannel; }

    /// §14.6: after naming a mic, or after an ambiguous tap, start listening
    /// again for the next one.
    void reset();

private:
    int numChannels;
    TapResult result = TapResult::Listening;
    int tappedChannel = -1;

    /// How long each channel has been the only one above the tap threshold.
    std::vector<double> qualifyingSeconds;
};

} // namespace mma
