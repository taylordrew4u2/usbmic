#include "PreflightThroughputTest.h"
#include <algorithm>
#include <sstream>

namespace mma {

double PreflightThroughputTest::requiredBytesPerSecond (int numChannels, double sampleRate, int bytesPerSample,
                                                        double videoBytesPerSecond) noexcept
{
    // channels * sampleRate * bytesPerSample * 2 (card write + mix file overhead),
    // then the video on top -- written once, so not doubled.
    return static_cast<double> (numChannels) * sampleRate * static_cast<double> (bytesPerSample) * 2.0
         + (videoBytesPerSecond > 0.0 ? videoBytesPerSecond : 0.0);
}

double PreflightThroughputTest::sustainedMinimum (const std::vector<double>& rollingWindowBytesPerSec) noexcept
{
    if (rollingWindowBytesPerSec.empty())
        return 0.0;
    return *std::min_element (rollingWindowBytesPerSec.begin(), rollingWindowBytesPerSec.end());
}

PreflightResult PreflightThroughputTest::evaluate (const std::vector<double>& rollingWindowBytesPerSec,
                                                   int numChannels, double sampleRate, int bytesPerSample,
                                                   double videoBytesPerSecond)
{
    return evaluateMeasured (sustainedMinimum (rollingWindowBytesPerSec),
                             numChannels, sampleRate, bytesPerSample, videoBytesPerSecond);
}

PreflightResult PreflightThroughputTest::evaluateMeasured (double sustainedMinBytesPerSec,
                                                           int numChannels, double sampleRate,
                                                           int bytesPerSample, double videoBytesPerSecond)
{
    PreflightResult result;
    result.sustainedMinBytesPerSec = sustainedMinBytesPerSec;
    result.requiredBytesPerSec = requiredBytesPerSecond (numChannels, sampleRate, bytesPerSample,
                                                         videoBytesPerSecond);

    const double gate = result.requiredBytesPerSec * kRequiredMultiplier;
    result.passed = result.sustainedMinBytesPerSec >= gate;

    if (! result.passed)
    {
        std::ostringstream oss;
        oss << "This card is too slow for this recording. Measured "
            << static_cast<long long> (result.sustainedMinBytesPerSec / (1024 * 1024))
            << " MB/s, needs at least "
            << static_cast<long long> (gate / (1024 * 1024)) << " MB/s.";

        // §10.6: what happened, then what to do. When a camera is what pushed
        // the requirement over the card's measured speed, switching it off is a
        // real way out and the user cannot guess it from a number.
        if (videoBytesPerSecond > 0.0)
        {
            const double audioOnlyGate = requiredBytesPerSecond (numChannels, sampleRate, bytesPerSample)
                                       * kRequiredMultiplier;

            if (sustainedMinBytesPerSec >= audioOnlyGate)
                oss << " Turning the cameras off would bring it back within range.";
        }

        result.reason = oss.str();
    }

    return result;
}

std::string PreflightThroughputTest::formatRemainingTime (uint64_t freeBytes, double requiredBytesPerSecPerFile) noexcept
{
    if (requiredBytesPerSecPerFile <= 0.0)
        return "0h 00m";

    const double totalSeconds = static_cast<double> (freeBytes) / requiredBytesPerSecPerFile;
    const long long totalMinutes = static_cast<long long> (totalSeconds / 60.0);
    const long long hours = totalMinutes / 60;
    const long long minutes = totalMinutes % 60;

    std::ostringstream oss;
    oss << hours << "h " << (minutes < 10 ? "0" : "") << minutes << "m";
    return oss.str();
}

} // namespace mma
