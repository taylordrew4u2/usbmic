#pragma once

namespace mma {

/// What the app does with a request to quit, when the answer depends on
/// whether the last take's files are finished.
///
/// A take's camera movie is only playable once the camera says it has
/// finished writing it, so quitting waits for that -- and the camera side
/// already gives up by itself after 15 seconds (CameraController). A second
/// Cmd-Q or close click inside that window used to force the quit anyway,
/// tearing the camera down in the middle of the write and leaving a truncated
/// movie: the one outcome the wait existed to prevent. A repeat is now
/// ignored until the bound has passed; only then is it the user's escape
/// hatch for something that wedged beyond it.
///
/// Pure bookkeeping on a millisecond clock the caller supplies, so every case
/// is testable without a message loop or a camera.
class QuitGate
{
public:
    enum class Decision
    {
        QuitNow,      ///< nothing left to finish: quit
        StartWaiting, ///< files still finishing: poll, and quit once they are
        Ignore,       ///< a repeat while the files are still inside their bound
        ForceQuit,    ///< a repeat after the bound: stop waiting
    };

    /// The camera's own 15 s finalization bound, plus room for the stop that
    /// a quit mid-take performs first and for the poll that notices the
    /// timeout. Past this, still waiting means something is wedged.
    static constexpr double kDefaultBoundMs = 20000.0;

    explicit QuitGate (double boundMs = kDefaultBoundMs) noexcept : bound (boundMs) {}

    /// A quit was asked for. `readyToQuit` is whether everything is finished.
    Decision onQuitRequested (bool readyToQuit, double nowMs) noexcept;

    /// While pending: true once the files are finished, and the gate is
    /// cleared. The caller then quits.
    bool onPoll (bool readyToQuit) noexcept;

    bool isPending() const noexcept { return pending; }

private:
    double bound;
    bool pending = false;
    double firstRequestMs = 0.0;
};

} // namespace mma
