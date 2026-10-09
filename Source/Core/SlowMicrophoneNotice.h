#pragma once

namespace mma {

/// §5.4: when one microphone is far enough behind the rest in the headphones
/// that the activity log names it.
///
/// The headline headphone figure is the quickest microphone's own path, since
/// no channel waits for any other. One interface that another app holds at a
/// 2048-frame buffer is 40 ms late in its own channel while the figure reads
/// 5 ms, and that is never left unsaid.
///
/// What it must not do is name a microphone for being a block behind a rig
/// that is past the ceiling as a whole. The buffer ladder takes every path
/// past 10 ms at 128 frames and up, and the rule this replaced -- the slowest
/// past the ceiling and a millisecond behind -- then told the user that
/// whichever interface reported 48 frames more latency "runs at a larger
/// buffer than the others" and to close apps and replug it. The ladder's own
/// notice has already said what the whole rig costs, the slowest microphone
/// included. So a microphone is named only when it alone is past the ceiling
/// (the rest are within it), or when it is several milliseconds behind them
/// wherever they are.
///
/// Pure arithmetic on the two figures the coordinator gives
/// (getMonitoringLatencyMs, getSlowestMonitoringLatencyMs).
struct SlowMicrophoneNotice
{
    /// §5.4's ceiling.
    static constexpr double kCeilingMs = 10.0;

    /// Less than this behind the rest is not a different path at all: an
    /// input latency or a block a few dozen frames apart.
    static constexpr double kNoticeableMs = 1.0;

    /// Behind the rest by this much, it is said even when the rest are past
    /// the ceiling too: a buffer held larger than the rig's, not the rig's
    /// own size.
    static constexpr double kFarBehindMs = 5.0;

    static constexpr bool isFarBehind (double quickestMs, double slowestMs) noexcept
    {
        const double gap = slowestMs - quickestMs;

        if (slowestMs <= kCeilingMs || gap < kNoticeableMs)
            return false;

        return quickestMs <= kCeilingMs || gap >= kFarBehindMs;
    }
};

} // namespace mma
