#pragma once

#include <vector>
#include <cstdint>

namespace mma {

struct DeviceRateCapability
{
    int deviceIndex = 0;
    std::vector<uint32_t> supportedRates; // e.g. {44100, 48000, 96000}

    /// The rate this device is running at right now; 0 when the backend cannot
    /// say.
    ///
    /// A device advertising a rate is not the same as a device that will switch
    /// to it. An interface clock-locked to 44.1 kHz, or one another process has
    /// a claim on, lists 48 kHz among its capabilities and then refuses the
    /// write -- and refusing to open is how that reached a user: a working rig,
    /// a plugged-in microphone, and a take that would not start over a number
    /// nobody had chosen.
    uint32_t currentRate = 0;
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
///
/// With one exception, which costs nothing and prevents a take that will not
/// start: when every device is ALREADY running at one common rate, that rate is
/// chosen even if a higher one is also common. Highest-common is only better on
/// paper -- it is worth nothing if the hardware refuses to switch, and the
/// quality difference between 44.1 and 48 kHz is inaudible next to a recording
/// that did not happen.
class SampleRateNegotiator
{
public:
    static constexpr uint32_t kRateCap = 48000;

    static RateNegotiationResult negotiate (const std::vector<DeviceRateCapability>& devices);
};

} // namespace mma
