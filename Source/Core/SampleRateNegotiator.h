#pragma once

#include <vector>
#include <cstdint>

namespace mma {

struct DeviceRateCapability
{
    int deviceIndex = 0;
    std::vector<uint32_t> supportedRates; // e.g. {44100, 48000, 96000}
};

struct RateNegotiationResult
{
    uint32_t chosenRate = 48000;
    // Devices that cannot natively reach chosenRate and must be resampled.
    std::vector<int> devicesNeedingResample;
};

/// §2.2: choose the highest sample rate common to all devices, capped at 48kHz.
/// Never rejects a device for rate reasons -- devices that can't reach the chosen
/// rate are simply flagged for resampling.
class SampleRateNegotiator
{
public:
    static constexpr uint32_t kRateCap = 48000;

    static RateNegotiationResult negotiate (const std::vector<DeviceRateCapability>& devices);
};

} // namespace mma
