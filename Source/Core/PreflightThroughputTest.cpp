#include "PreflightThroughputTest.h"
#include <algorithm>
#include <sstream>

namespace mma {

double PreflightThroughputTest::requiredBytesPerSecond (int numChannels, double sampleRate, int bytesPerSample) noexcept
{
    // channels * sampleRate * bytesPerSample * 2 (card write + mix file overhead)
    return static_cast<double> (numChannels) * sampleRate * static_cast<double> (bytesPerSample) * 2.0;
}

double PreflightThroughputTest::sustainedMinimum (const std::vector<double>& rollingWindowBytesPerSec) noexcept
{
    if (rollingWindowBytesPerSec.empty())
        return 0.0;
    return *std::min_element (rollingWindowBytesPerSec.begin(), rollingWindowBytesPerSec.end());
}

PreflightResult PreflightThroughputTest::evaluate (const std::vector<double>& rollingWindowBytesPerSec,
                                                   int numChannels, double sampleRate, int bytesPerSample)
{
    PreflightResult result;
    result.sustainedMinBytesPerSec = sustainedMinimum (rollingWindowBytesPerSec);
    result.requiredBytesPerSec = requiredBytesPerSecond (numChannels, sampleRate, bytesPerSample);

    const double gate = result.requiredBytesPerSec * kRequiredMultiplier;
    result.passed = result.sustainedMinBytesPerSec >= gate;

    if (! result.passed)
    {
        std::ostringstream oss;
        oss << "This card is too slow for this recording. Measured "
            << static_cast<long long> (result.sustainedMinBytesPerSec / (1024 * 1024))
            << " MB/s, needs at least "
            << static_cast<long long> (gate / (1024 * 1024)) << " MB/s.";
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
