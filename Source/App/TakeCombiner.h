#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "../Core/CombinedTakePlan.h"

namespace mma {

/// Runs the combining step after a take, off the message thread.
///
/// Muxing a four-hour take is minutes of work even when the picture is only
/// copied, so none of it may happen on the thread drawing the meters. It also
/// must never be able to cost anyone a recording: every input file is already
/// closed and complete before this starts, nothing here writes to them, and a
/// failure leaves the take exactly as it was -- separate, complete, and
/// playable. The combined file is a convenience, and a convenience that can
/// take the take down with it is not one.
class TakeCombiner
{
public:
    TakeCombiner();
    ~TakeCombiner();

    /// How the last run ended, in the app's voice (§10.6).
    struct Status
    {
        bool running = false;
        int done = 0;
        int total = 0;
        juce::String problem;   ///< empty unless something went wrong
        juce::StringArray written; ///< file names of the combined files that exist
    };

    /// Starts combining. Returns immediately; poll getStatus().
    ///
    /// A run already in flight is never doubled: two takes combining at once
    /// would fight for the same cores the next recording needs. The second
    /// take waits instead -- it is queued behind the first on the same worker,
    /// this returns true, and the run's status covers both. It used to return
    /// false and drop the plan, and the app never asked twice, so a take
    /// stopped while the last one was still muxing silently never got its
    /// combined file.
    bool start (const juce::File& sessionFolder, const CombinedTakePlan& plan);

    bool isRunning() const;
    Status getStatus() const;

    /// Requests that any ffmpeg child stop. Returns immediately; the detached
    /// worker owns its remaining cleanup and cannot refer back to this object.
    void cancel() noexcept;

    /// The ffmpeg this will use, or empty when none was found. A find is
    /// cached; a miss is not, because the user may install ffmpeg while the
    /// app runs -- the message says the next take will combine, so it must.
    /// Misses are re-checked at most every kFfmpegRetryMs here, since this is
    /// asked from the message thread and a PATH probe spawns a process;
    /// start() always looks again.
    juce::String findFfmpeg();

    static constexpr juce::uint32 kFfmpegRetryMs = 10000;

    /// Test seam: run this instead of looking for ffmpeg on the machine.
    void setFfmpegOverride (const juce::String& path) { ffmpegOverride = path; }

    /// Test seam: look in these places instead of ffmpegSearchPaths().
    void setFfmpegSearchPathsForTesting (std::vector<std::string> paths)
    {
        searchPathsForTesting = std::move (paths);
        hasSearchPathsForTesting = true;
    }

private:
    juce::String ffmpegOverride;
    std::vector<std::string> searchPathsForTesting;
    bool hasSearchPathsForTesting = false;
    juce::String resolvedFfmpeg;
    bool haveMissed = false;
    juce::uint32 lastMissMs = 0;

    juce::String probeFfmpeg();

    struct QueuedTake
    {
        juce::File sessionFolder;
        CombinedTakePlan plan;
    };

    struct RunState
    {
        std::atomic<bool> running { false };
        std::atomic<bool> cancelling { false };
        mutable std::mutex statusLock;
        Status status;

        /// Takes stopped while this run was busy, in the order they stopped.
        /// Guarded by statusLock, which is also held while the worker decides
        /// it has finished -- so a take is either picked up by this run or
        /// finds it over and starts its own. Never neither.
        std::deque<QueuedTake> queued;
    };

    std::shared_ptr<RunState> runState;

    static void run (std::shared_ptr<RunState> state,
                     juce::File sessionFolder,
                     CombinedTakePlan plan,
                     juce::String ffmpeg);

    static void combineTake (const std::shared_ptr<RunState>& state,
                             const juce::File& sessionFolder,
                             const CombinedTakePlan& plan,
                             const juce::String& ffmpeg,
                             int& failures,
                             juce::String& firstFailureDetail);
};

} // namespace mma
