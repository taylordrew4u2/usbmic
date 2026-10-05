#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "../Core/StreamingTargets.h"

namespace mma {

/// How one podcast-ready copy turned out.
struct PodcastExportResult
{
    /// True only once the copy is complete on disk under its final name.
    bool written = false;

    /// The finished copy. Unset unless `written`.
    juce::File output;

    /// The mix as it was measured, and what was done to it.
    double measuredLufs = 0.0;
    double measuredTruePeakDbtp = 0.0;
    double gainDb = 0.0;
    bool limitedByTruePeak = false;

    /// Where the copy lands: the target, or short of it when the peaks would
    /// not allow the whole of the gain.
    double resultingLufs = 0.0;

    /// One sentence in the app's voice (§10.6): what was saved, or why
    /// nothing was. Never empty.
    juce::String message;
};

/// "MIX - Apple-Podcasts.wav" for "MIX.wav" and "Apple Podcasts". The target's
/// name goes through SessionFolderNaming, because it ends up in a file name on
/// whatever card the take is on, and "Broadcast (EBU R128)" is not a name every
/// filesystem will take.
juce::String podcastCopyFileName (const juce::String& mixFileName, const std::string& targetName);

/// Writes a loudness-normalised copy of a finished take's mix beside it.
///
/// Reads `mixFileName` in `sessionFolder` -- and MIX_001.wav onwards, when a
/// long take split -- measures it with LoudnessMeter, asks adviseForTarget()
/// what gain the target wants (already capped so the true peak stays under the
/// platform's ceiling), and writes the mix again with that gain as one 24-bit
/// WAV at the same rate. The take's own files are only ever read.
///
/// Nothing is written for a take too short or too quiet to measure: a copy
/// "normalised" from a meaningless figure would be the one file in the folder
/// that is wrong on purpose. The result says why instead.
///
/// The copy is written under a working name and renamed into place once
/// complete, so a copy cut short by `cancel`, a full card or a crash never
/// sits in the folder looking like a finished one.
///
/// Blocking, and minutes of reading for a long take: never call it on the
/// audio or message thread. PodcastExporter is how the app runs it.
PodcastExportResult exportPodcastCopy (const juce::File& sessionFolder,
                                       const juce::String& mixFileName,
                                       const StreamingTarget& target,
                                       const std::atomic<bool>& cancel);

/// Runs exportPodcastCopy() after each take, on a thread of its own.
///
/// Stop must never wait on this: it is started once the take's files are
/// closed, and returns immediately. Takes stopped while one is still being
/// exported queue behind it rather than racing it for the disk the next take
/// is recording to.
class PodcastExporter
{
public:
    PodcastExporter();

    /// Cancels, and waits a short bounded while for the worker to clear away
    /// its unfinished copy. Never joins: a card that stopped answering can
    /// hold a read forever, and quitting must not wait on that.
    ~PodcastExporter();

    /// Queues one take. False when quitting, or when `targetName` names no
    /// target.
    bool start (const juce::File& sessionFolder, const juce::String& mixFileName,
                const std::string& targetName);

    /// Results finished since the last call, oldest first. For the message
    /// thread, which is the only place that may report them.
    std::vector<PodcastExportResult> collectFinished();

    bool isRunning() const;

    /// Stops the worker at its next block and refuses further work. Returns
    /// immediately.
    void cancel() noexcept;

    /// Waits up to `timeoutMs` for the worker to finish. True once idle.
    bool waitUntilIdle (int timeoutMs);

    /// The longest the destructor (and so quitting) will wait.
    static constexpr int kShutdownWaitMs = 2000;

private:
    struct Job
    {
        juce::File sessionFolder;
        juce::String mixFileName;
        StreamingTarget target;
    };

    struct State
    {
        mutable std::mutex lock;
        std::condition_variable idle;
        std::deque<Job> queued;
        std::vector<PodcastExportResult> finished;
        bool running = false;
        std::atomic<bool> cancelling { false };
    };

    // Shared with the detached worker, which may outlive this object.
    std::shared_ptr<State> state;

    static void run (std::shared_ptr<State> state);
};

} // namespace mma
