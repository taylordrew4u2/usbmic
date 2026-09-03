#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
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
    /// A run already in flight is left alone and this returns false: two takes
    /// combining at once would fight for the same cores the next recording
    /// needs, and the second one can wait.
    bool start (const juce::File& sessionFolder, const CombinedTakePlan& plan);

    /// Blocks until the current run has finished. Used on shutdown -- a process
    /// left running past the app's exit writes a file nobody is waiting for.
    void waitForCompletion();

    bool isRunning() const { return running.load(); }
    Status getStatus() const;

    /// The ffmpeg this will use, or empty when none was found. Resolved once
    /// and cached, since the answer cannot change while the app runs.
    juce::String findFfmpeg();

    /// Test seam: run this instead of looking for ffmpeg on the machine.
    void setFfmpegOverride (const juce::String& path) { ffmpegOverride = path; }

private:
    juce::String ffmpegOverride;
    juce::String resolvedFfmpeg;
    bool haveResolved = false;

    std::atomic<bool> running { false };
    std::atomic<bool> cancelling { false };

    mutable std::mutex statusLock;
    Status status;

    std::unique_ptr<std::thread> worker;

    void run (juce::File sessionFolder, CombinedTakePlan plan, juce::String ffmpeg);
};

} // namespace mma
