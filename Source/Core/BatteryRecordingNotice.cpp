#include "BatteryRecordingNotice.h"

namespace mma {

bool BatteryRecordingNotice::tick (bool recording, double elapsedSeconds,
                                   const std::function<PowerSource()>& readPowerSource)
{
    if (! recording)
    {
        // Re-armed for the next take.
        wasRecording = false;
        warnedThisTake = false;
        return false;
    }

    const bool takeJustStarted = ! wasRecording;
    wasRecording = true;

    if (warnedThisTake || readPowerSource == nullptr)
        return false;

    sinceLastRead += elapsedSeconds;

    if (! takeJustStarted && sinceLastRead < kRecheckSeconds)
        return false;

    sinceLastRead = 0.0;

    // Unknown is not battery: a Mac that will not say must not be nagged.
    if (readPowerSource() != PowerSource::Battery)
        return false;

    warnedThisTake = true;
    return true;
}

} // namespace mma
