#pragma once

namespace mma {

/// §6.1: the recorded MIX.wav file's limiter. A separate instance from
/// MonitorBus's safety limiter -- this one is never affected by the monitor's
/// runaway cut or global mute, and the monitor is never affected by this one.
/// Ceiling -1dBFS; no runaway/mute semantics apply to the recording path.
class MixBusLimiter
{
public:
    static constexpr float kCeilingDb = -1.0f;

    /// Zero-lookahead hard ceiling applied to the trimmed, summed mix-file signal.
    static float processSample (float sumInputSample) noexcept;
};

} // namespace mma
