#pragma once
#include <cstddef>
#include <vector>

namespace mma {

/// ITU-R BS.1770-4 loudness, which is the measurement every streaming platform
/// normalises against.
///
/// Peak level -- which is all this app measured before -- says nothing about
/// how loud something sounds. A take peaking at -3 dBFS can be six decibels
/// quieter to the ear than another peaking at the same number, and it is the
/// second one the platform will turn down. So the figure that decides what a
/// listener hears is this one, and it was not being measured at all.
///
/// Three stages, in the standard's order:
///
///   1. K-weighting: a high-shelf and a high-pass in series, standing in for
///      the head and the ear's indifference to the very bottom.
///   2. Mean square over 400 ms blocks overlapping by 75%.
///   3. Two gates -- an absolute one at -70 LUFS and a relative one 10 LU below
///      the ungated mean -- so silence between sentences does not drag the
///      figure down. Ungated, a take of someone talking with pauses measures
///      quieter than the same voice without them, which is the opposite of
///      what the number is for.
///
/// Fed from the writer thread rather than the audio callback: §11 forbids this
/// much arithmetic per block in the callback, and loudness is a property of the
/// take rather than of the moment, so it can afford to arrive late.
class LoudnessMeter
{
public:
    /// Below this, a block is silence and is excluded outright (BS.1770 gate 1).
    static constexpr double kAbsoluteGateLufs = -70.0;

    /// The relative gate sits this far below the ungated mean (BS.1770 gate 2).
    static constexpr double kRelativeGateLu = -10.0;

    /// What "no measurement yet" reads as. Also what an entirely silent take
    /// measures, since every one of its blocks fails the absolute gate.
    static constexpr double kSilenceLufs = -200.0;

    explicit LoudnessMeter (double sampleRate);

    /// Clears every block and restarts the filters. Call between takes.
    void reset();

    /// One channel's samples. Mono throughout, because everything this app
    /// writes is mono -- see StreamingTargets for why that matters more than it
    /// looks like it should.
    void process (const float* samples, size_t numSamples);

    /// Gated integrated loudness over everything fed so far, in LUFS.
    /// kSilenceLufs when nothing has passed the absolute gate.
    double getIntegratedLufs() const;

    /// The highest sample magnitude seen, in dBFS. Not a true peak -- see
    /// truePeakDbtp() for that -- but exact for what it claims.
    double getSamplePeakDbfs() const;

    /// True peak in dBTP, estimated by 4x oversampling as BS.1770 Annex 2
    /// specifies. The number matters because the sample peak of a signal is not
    /// its analogue peak: a waveform can pass between two samples at a level
    /// higher than either, and a file that looks like it sits at -1 dBFS can
    /// clip a platform's decoder. That is what a true-peak ceiling is for.
    double getTruePeakDbtp() const;

    /// How many 400 ms blocks have been measured. Loudness over a very short
    /// take is not meaningful, and this is what lets a caller say so.
    int getBlockCount() const { return static_cast<int> (blockLoudness.size()); }

private:
    /// One biquad, direct form I. Two of these in series are the K-weighting.
    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

        void reset() { x1 = x2 = y1 = y2 = 0.0; }

        double process (double x) noexcept
        {
            const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            return y;
        }
    };

    double sampleRate;
    Biquad shelf, highPass;

    size_t blockSamples = 0;   // 400 ms
    size_t hopSamples = 0;     // 100 ms, giving the standard's 75% overlap
    size_t sinceLastBlock = 0;

    // A ring of the last 400 ms of K-weighted squares, so overlapping blocks
    // share the samples they overlap on rather than the caller buffering.
    std::vector<double> window;
    size_t windowPos = 0;
    size_t windowFilled = 0;
    double windowSum = 0.0;

    std::vector<double> blockLoudness; // mean square per block, linear

    double samplePeak = 0.0;
    double truePeak = 0.0;

    // The last few input samples, for the oversampling filter to look back on.
    double tpHistory[4] = { 0.0, 0.0, 0.0, 0.0 };

    void buildKWeighting();
    void pushSquare (double kWeighted);
    void updateTruePeak (double sample);
};

} // namespace mma
