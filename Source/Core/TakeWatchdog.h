#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mma {

/// One reading of everything that can go wrong in the middle of a take, in
/// the terms the user thinks in: which microphones are sending sound, which
/// cameras are still there, whether the disk is keeping up.
struct TakeHealth
{
    struct Mic    { std::string name; bool live = true; };
    struct Camera { std::string name; bool present = true; };

    std::vector<Mic> mics;
    std::vector<Camera> cameras;

    /// The camera controller's own complaint, empty when it has none.
    std::string cameraProblem;
    /// The monitor path's complaint (a device that would not open, a rate
    /// refused), empty when healthy.
    std::string monitorProblem;

    /// Audio frames this app dropped so far in the take. Any increase is
    /// sound that is gone.
    uint64_t framesDropped = 0;
    /// True while the writer is falling behind (ring buffer past its warning
    /// mark) and true while it has given up the stems and kept only the mix.
    bool writerBehind = false;
    bool mixOnly = false;

    /// Room left on the destination, in seconds. Negative when unknown.
    double remainingSeconds = -1.0;
};

struct TakeAlert
{
    enum class Kind
    {
        MicLost, MicBack, CameraLost, CameraBack, CameraTrouble,
        AudioDropped, WriterBehind, MixOnly, MonitorTrouble,
        TenMinutesLeft, TwoMinutesLeft,
    };

    Kind kind;
    /// §10.6: what happened, then what it means, in plain language.
    std::string message;
    /// True for the good news: something that was lost has come back.
    bool recovery = false;
};

/// §0.1 and §6.5: nothing that goes wrong mid-take may go unsaid. The main
/// screen has always shown the state; this turns a CHANGE in it into an
/// alert, once per change, so a pop-up can name it the moment it happens
/// rather than leaving the user to notice a dashed skull.
///
/// Plain data in and alerts out, no clock and no UI, so every rule here is
/// held by a test.
class TakeWatchdog
{
public:
    /// The take has started; this is what healthy looks like. Nothing in the
    /// baseline is an alert, however bad -- a mic that was already dead when
    /// record was pressed was on screen before the press.
    void beginTake (const TakeHealth& baseline);
    void endTake();
    bool isWatching() const { return watching; }

    /// Compares against the last reading and reports what changed for the
    /// worse, and what came back. Empty when nothing changed.
    std::vector<TakeAlert> observe (const TakeHealth& now);

    static constexpr double kTenMinutes = 600.0;
    static constexpr double kTwoMinutes = 120.0;

private:
    bool watching = false;
    TakeHealth last;
    bool warnedTenMinutes = false;
    bool warnedTwoMinutes = false;
};

} // namespace mma
