#pragma once

namespace mma {

enum class SystemThermalState
{
    Unknown,
    Nominal,
    Fair,
    Serious,
    Critical,
};

/// Reads the OS's own thermal-pressure state. Unknown is deliberately not
/// treated as healthy: callers can distinguish unavailable evidence from a
/// platform that affirmatively reports nominal operation.
SystemThermalState getSystemThermalState() noexcept;

inline bool isSystemThermallyThrottled() noexcept
{
    const auto state = getSystemThermalState();
    return state == SystemThermalState::Serious || state == SystemThermalState::Critical;
}

} // namespace mma
