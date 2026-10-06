#pragma once

#include <functional>

namespace mma {

/// What is powering the computer, as far as the OS will say.
enum class PowerSource
{
    Unknown, ///< No answer, or a platform that does not report one.
    AC,      ///< Plugged in (a desktop always is).
    Battery
};

/// §6.6: the sleep assertions keep a plugged-in Mac awake with its lid shut,
/// but on battery macOS sleeps on lid close whatever an app asks -- and the
/// take stops with it. Nothing can prevent that, so the performer is told,
/// once per take: when a take starts on battery, or when the charger is
/// pulled part-way through one.
///
/// Pure bookkeeping on elapsed seconds the caller supplies; the OS is asked
/// through `readPowerSource` only while a take records, and at most every
/// kRecheckSeconds, so a status tick twice a second does not become a power
/// query twice a second.
class BatteryRecordingNotice
{
public:
    static constexpr double kRecheckSeconds = 5.0;

    static constexpr const char* kMessage =
        "Keep the lid open: on battery, closing it will stop the recording.";

    /// Advance by `elapsedSeconds`. True means say kMessage now. The first
    /// tick of a take always reads the power source; a take that has already
    /// been warned is not warned again, whatever the power does after.
    bool tick (bool recording, double elapsedSeconds,
               const std::function<PowerSource()>& readPowerSource);

private:
    bool wasRecording = false;
    bool warnedThisTake = false;
    double sinceLastRead = 0.0;
};

} // namespace mma
