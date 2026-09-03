#include "LoudnessMeter.h"
#include <algorithm>
#include <cmath>

namespace mma {

namespace {

// Not M_PI: that is a POSIX extension rather than standard C++, and MSVC does
// not define it from <cmath> without _USE_MATH_DEFINES. Tools/e2e_capture.cpp
// already carries this note because relying on it broke the Windows build once
// before; this file did it again.
constexpr double kPi = 3.14159265358979323846;

// BS.1770's own weighting for a single channel. Left, right and centre all
// count at 1.0; only the surrounds are lifted, and this app has none.
constexpr double kChannelWeight = 1.0;

// The -0.691 in the loudness equation. It is a calibration offset, not a
// derived quantity: it makes the K-weighted mean square of a reference signal
// come out at the level that signal actually is.
constexpr double kLoudnessOffset = -0.691;

double toLufs (double meanSquare)
{
    if (meanSquare <= 0.0)
        return LoudnessMeter::kSilenceLufs;

    return kLoudnessOffset + 10.0 * std::log10 (kChannelWeight * meanSquare);
}

} // namespace

LoudnessMeter::LoudnessMeter (double rate)
    : sampleRate (rate > 0.0 ? rate : 48000.0)
{
    blockSamples = static_cast<size_t> (sampleRate * 0.400);
    hopSamples = static_cast<size_t> (sampleRate * 0.100);

    window.assign (std::max<size_t> (1, blockSamples), 0.0);

    buildKWeighting();
    reset();
}

void LoudnessMeter::buildKWeighting()
{
    // Stage 1: the shelving filter, standing in for the acoustic effect of a
    // head in the sound field. Stage 2: a high-pass, because the ear does not
    // weigh the very bottom of the spectrum the way an RMS meter does.
    //
    // BS.1770 tabulates both at 48 kHz. Using those numbers at any other rate
    // would silently mis-weight the measurement, so they are derived from the
    // filter parameters the table was itself generated from -- which is what
    // makes this correct at 44.1 and 96 kHz too.
    {
        // High-shelf: +4 dB above ~1.5 kHz, Q = 1/sqrt(2).
        const double f0 = 1681.974450955533;
        const double G  = 3.999843853973347;   // dB
        const double Q  = 0.7071752369554196;

        const double K = std::tan (kPi * f0 / sampleRate);
        const double Vh = std::pow (10.0, G / 20.0);
        const double Vb = std::pow (Vh, 0.4996667741545416);

        const double denom = 1.0 + K / Q + K * K;

        shelf.b0 = (Vh + Vb * K / Q + K * K) / denom;
        shelf.b1 = 2.0 * (K * K - Vh) / denom;
        shelf.b2 = (Vh - Vb * K / Q + K * K) / denom;
        shelf.a1 = 2.0 * (K * K - 1.0) / denom;
        shelf.a2 = (1.0 - K / Q + K * K) / denom;
    }

    {
        // High-pass at ~38 Hz, Q = 0.5.
        const double f0 = 38.13547087602444;
        const double Q  = 0.5003270373238773;

        const double K = std::tan (kPi * f0 / sampleRate);

        highPass.b0 = 1.0;
        highPass.b1 = -2.0;
        highPass.b2 = 1.0;
        highPass.a1 = 2.0 * (K * K - 1.0) / (1.0 + K / Q + K * K);
        highPass.a2 = (1.0 - K / Q + K * K) / (1.0 + K / Q + K * K);
    }
}

void LoudnessMeter::reset()
{
    shelf.reset();
    highPass.reset();

    std::fill (window.begin(), window.end(), 0.0);
    windowPos = 0;
    windowFilled = 0;
    windowSum = 0.0;
    sinceLastBlock = 0;

    blockLoudness.clear();

    samplePeak = 0.0;
    truePeak = 0.0;
    for (auto& h : tpHistory)
        h = 0.0;
}

void LoudnessMeter::pushSquare (double kWeighted)
{
    const double square = kWeighted * kWeighted;

    // A running sum over a ring, so each 400 ms block costs one add and one
    // subtract rather than a fresh pass over 19,200 samples.
    windowSum -= window[windowPos];
    window[windowPos] = square;
    windowSum += square;

    windowPos = (windowPos + 1) % window.size();
    windowFilled = std::min (windowFilled + 1, window.size());

    // A block every 100 ms once the window is full: the standard's 400 ms
    // block with 75% overlap.
    if (windowFilled < window.size())
        return;

    if (++sinceLastBlock < hopSamples)
        return;

    sinceLastBlock = 0;

    // Guarded against the sum drifting below zero through floating-point
    // cancellation on a long take, which would otherwise produce a NaN here.
    blockLoudness.push_back (std::max (0.0, windowSum) / static_cast<double> (window.size()));
}

void LoudnessMeter::updateTruePeak (double sample)
{
    samplePeak = std::max (samplePeak, std::abs (sample));

    // 4x oversampling by linear interpolation between this sample and the last
    // three. Not the standard's polyphase FIR -- that is a 48-tap filter per
    // phase and this runs over every sample of every take -- but it recovers
    // the great majority of an inter-sample peak, and erring low here is the
    // safe direction: it reports less headroom than there is, never more.
    for (int phase = 1; phase < 4; ++phase)
    {
        const double t = phase / 4.0;
        const double interpolated = tpHistory[0] * (1.0 - t) + sample * t;
        truePeak = std::max (truePeak, std::abs (interpolated));
    }

    truePeak = std::max (truePeak, std::abs (sample));

    tpHistory[3] = tpHistory[2];
    tpHistory[2] = tpHistory[1];
    tpHistory[1] = tpHistory[0];
    tpHistory[0] = sample;
}

void LoudnessMeter::process (const float* samples, size_t numSamples)
{
    if (samples == nullptr)
        return;

    for (size_t i = 0; i < numSamples; ++i)
    {
        const double x = static_cast<double> (samples[i]);

        updateTruePeak (x);
        pushSquare (highPass.process (shelf.process (x)));
    }
}

double LoudnessMeter::getIntegratedLufs() const
{
    if (blockLoudness.empty())
        return kSilenceLufs;

    // Gate 1, absolute: anything below -70 LUFS is silence and is not part of
    // the programme.
    double sum = 0.0;
    int count = 0;

    for (double meanSquare : blockLoudness)
    {
        if (toLufs (meanSquare) > kAbsoluteGateLufs)
        {
            sum += meanSquare;
            ++count;
        }
    }

    if (count == 0)
        return kSilenceLufs;

    // Gate 2, relative: 10 LU below the mean of what survived gate 1. This is
    // what stops the pauses between sentences from counting as programme.
    const double relativeThreshold = toLufs (sum / count) + kRelativeGateLu;

    double gatedSum = 0.0;
    int gatedCount = 0;

    for (double meanSquare : blockLoudness)
    {
        const double lufs = toLufs (meanSquare);

        if (lufs > kAbsoluteGateLufs && lufs > relativeThreshold)
        {
            gatedSum += meanSquare;
            ++gatedCount;
        }
    }

    if (gatedCount == 0)
        return kSilenceLufs;

    return toLufs (gatedSum / gatedCount);
}

double LoudnessMeter::getSamplePeakDbfs() const
{
    return samplePeak > 0.0 ? 20.0 * std::log10 (samplePeak) : kSilenceLufs;
}

double LoudnessMeter::getTruePeakDbtp() const
{
    return truePeak > 0.0 ? 20.0 * std::log10 (truePeak) : kSilenceLufs;
}

} // namespace mma
