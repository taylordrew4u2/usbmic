#pragma once

namespace mma {

/// When to try the microphones again after none of them would open.
///
/// A failed open used to be final. With nothing open the Record button says
/// "The microphones aren't open: ..." and asks the user to fix something --
/// set the interface's rate in Audio MIDI Setup, quit the app holding it,
/// wait for macOS to finish -- and then nothing noticed the fix: a rate or
/// hog-mode change fires no device-list notification, and a device that never
/// opened has no stream to listen on. Record stayed disabled until a replug,
/// a Settings change or a relaunch.
///
/// This retries, but only when NOTHING is open. A reopen closes every stream
/// first, so on a rig that is partly up it would cut the headphones and reset
/// the meters and channel analysis on every attempt. It backs off (3, 6, 12,
/// 24, then every 30 s) because each attempt can hold the message thread for
/// up to about a second per device.
///
/// Pure bookkeeping on elapsed seconds the caller supplies, so every case is
/// testable without a device.
class FailedOpenRetry
{
public:
    static constexpr double kFirstIntervalSeconds = 3.0;
    static constexpr double kMaxIntervalSeconds = 30.0;

    struct Situation
    {
        /// No take running and the last take's files are finished.
        bool idle = false;
        /// Microphones switched on for recording.
        int includedMicCount = 0;
        /// At least one input stream is open.
        bool monitoring = false;
        /// Microphone access is denied; the permission poll reopens on grant.
        bool permissionDenied = false;
    };

    /// Advance by `elapsedSeconds`. True means reopen the microphones now.
    bool tick (double elapsedSeconds, const Situation& situation) noexcept;

private:
    double interval = kFirstIntervalSeconds;
    double remaining = kFirstIntervalSeconds;
};

} // namespace mma
